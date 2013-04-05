"""tstools.pyx -- Pyrex bindings for the tstools library

This is being developed on a Mac, running OS X, and also tested on my Ubuntu
system at work.

I do not expect it to build (as it stands) on Windows, as it is making
assumptions that may not follow thereon.

It is my intent to worry about Windows after it works on the platforms that
I can test most easily!
"""

# ***** BEGIN LICENSE BLOCK *****
# Version: MPL 1.1
#
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
#
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
#
# The Original Code is the MPEG TS, PS and ES tools.
#
# The Initial Developer of the Original Code is Amino Communications Ltd.
# Portions created by the Initial Developer are Copyright (C) 2008
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
#   Tibs (tibs@berlios.de)
#
# ***** END LICENSE BLOCK *****

"""
On static libraries versus dynamic libraries
============================================
Up in the main C source directories, tstools builds a static library,
libtstools.a, and the tstools applications are statically linked against that.
This simplifies life in many ways, but particularly:

1. Building shared/dynamic libraries in a portable manner is, well, hard
   (Mac OS X is particularly different).
2. Using applications built agains shared libraries means either putting the
   libraries in "known" locations, or setting particular paths on which to look
   for them. Neither of these is nice for the user to have to worrry about.

Unfortunately, this complicates matters a little when wrapping the aforesaid
static library with Pyrex/Cython. In an ideal world, I'd have a separate Python
module (.pyx file) for each "chunk" of tstools functionality (es, ts, pes,
etc.). However, if I link each of those against the static library, each gets
its own copy of said library. Again, this might not be *too* much problem
(space issues aside), but it totally fails if there is static data being used
withing the library -- each Python module would get its own copy. See
printing.c for why this is not a good thing...

So, this means that tstools.pyx remains a monolithic wrapper for the whole of
libtstools.a. I still think Pyrex/Cython is a better way to go than the other
choices, but perhaps not as elegant as I'd wish.
"""

import sys
import array

from common cimport FILE, EOF, stdout, fopen, fclose, fileno
from common cimport errno, strerror, free
from common cimport const_void_ptr
from common cimport PyString_FromStringAndSize, PyString_AsStringAndSize, \
                    PyObject_AsReadBuffer
from common cimport uint8_t, uint16_t, uint32_t, uint64_t
from common cimport int8_t, int16_t, int32_t, int64_t
from common cimport offset_t, byte, PID

cimport cwrapper

from cwrapper cimport ES, ES_p, ES_offset, ES_unit, ES_unit_p
from cwrapper cimport TS_reader, TS_reader_p, pidint_list, pidint_list_p
from cwrapper cimport pmt_stream, pmt_stream_p, pmt, pmt_p

# Is this the best thing to do?
class TSToolsException(Exception):
    pass

# =============================================================================
# Printing redirection
# =============================================================================

from common cimport const_char_ptr, va_list

cdef extern from "Python.h":
    # Write the output string described by format to sys.stdout. No exceptions
    # are raised, even if truncation occurs (see below).  
    #
    # format should limit the total size of the formatted output string to 1000
    # bytes or less – after 1000 bytes, the output string is truncated. In
    # particular, this means that no unrestricted “%s” formats should occur;
    # these should be limited using “%.<N>s” where <N> is a decimal number
    # calculated so that <N> plus the maximum size of other formatted text does
    # not exceed 1000 bytes. Also watch out for “%f”, which can print hundreds
    # of digits for very large numbers.
    #
    # If a problem occurs, or sys.stdout is unset, the formatted message is
    # written to the real (C level) stdout.
    void PySys_WriteStdout(const_char_ptr format, ...)

    # Output not more than size bytes to str according to the format string
    # format and the variable argument list va. Unix man page vsnprintf(2).
    int PyOS_vsnprintf(char *str, int size, const_char_ptr format, va_list va)

cdef void our_print_msg(const_char_ptr text):
    PySys_WriteStdout('%s',text)

cdef void our_format_msg(const_char_ptr format, va_list arg_ptr):
    cdef int err
    cdef char buffer[1000]
    PyOS_vsnprintf(buffer, 1000, format, arg_ptr)
    PySys_WriteStdout('%s',buffer)

cdef void our_flush():
    pass

def setup_printing():
    cdef int err
    err = cwrapper.redirect_output(our_print_msg, our_print_msg,
                          our_format_msg, our_format_msg, our_flush)
    if err:
        raise TSToolsException, 'Setting output redirection FAILED'

cdef void our_doctest_print_msg(const_char_ptr text):
    print 'YY ' + text,

cdef void our_doctest_format_msg(const_char_ptr format, va_list arg_ptr):
    cdef int err
    cdef char buffer[1000]
    PyOS_vsnprintf(buffer, 1000, format, arg_ptr)
    print 'XX ' + buffer,

def setup_printing_for_doctest():
    cdef int err
    err = cwrapper.redirect_output(our_doctest_print_msg, our_doctest_print_msg,
                          our_doctest_format_msg, our_doctest_format_msg, our_flush)
    if err:
        raise TSToolsException, 'Setting doctest output redirection FAILED'
    else:
        print 'Printing redirected for doctest'

def test_printing():
    cwrapper.print_msg('Message\n')
    cwrapper.print_err('Error\n')
    #cwrapper.fprint_msg('Message "%s"\n','Fred')
    #cwrapper.fprint_err('Error "%s"\n','Fred')

def test_c_printing():
    cwrapper.test_C_printing()

# =============================================================================
# ES matters
# =============================================================================

cdef _hexify_C_byte_array(byte *bytes, int bytes_len):
    """Return a representation of a (byte) array as a hex values string.

    Doesn't leave any spaces between hex bytes.
    """
    words = []
    for 0 <= ii < bytes_len:
        words.append('\\x%02x'%bytes[ii])
    return ''.join(words)

cdef class ESOffset:
    """An offset within an ES file.

    If the ES unit was read directly from a raw ES file, then a simple file
    offset is sufficient.

    However, if we're reading from a PS or TS file (via the PES reading layer),
    then we have the offset of the PES packet, and then the offset of the ES
    unit therein.
    
    We *could* just use a tuple for this, but it's nice to have a bit more
    documentation self-evident.
    """

    # Keep the original names, even though they're not very Pythonic
    cdef readonly long long infile      # Hoping this is 64 bit...
    cdef readonly int       inpacket

    def __cinit__(self, infile=0, inpacket=0):
        self.infile = infile
        self.inpacket = inpacket

    def __init__(self, infile=0, inpacket=0):
        pass

    def __str__(self):
        """Return a fairly compact and (relatively) self-explanatory format
        """
        return '%d+%d'%(self.infile,self.inpacket)

    def __repr__(self):
        """Return something we could be recreated from.
        """
        return 'ESOffset(infile=%d,inpacket=%d)'%(self.infile,self.inpacket)

    def formatted(self):
        """Return a representation that is similar to that returned by the C tools.

        Beware that this is <inpacket>+<infile>, which is reversed from the ``repr``.
        """
        return '%08d/%08d'%(self.inpacket,self.infile)

    def report(self):
        print 'Offset %d in packet at offset %d in file'%(self.inpacket,self.infile)

    def __cmp__(self,other):
        if self.infile > other.infile:
            return 1
        elif self.infile < other.infile:
            return -1
        elif self.inpacket > other.inpacket:
            return 1
        elif self.inpacket < other.inpacket:
            return -1
        else:
            return 0

cdef same_ES_unit(ES_unit_p this, ES_unit_p that):
    """Two ES units do not need to be at the same place to be the same.
    """
    if this.data_len != that.data_len:
        return False
    for 0 <= ii < this.data_len:
        if this.data[ii] != that.data[ii]:
            return False
    return True

cdef class ESUnit       # Forward declaration
cdef object compare_ESUnits(ESUnit this, ESUnit that, int op):
    """op is 2 for ==, 3 for !=, other values not allowed.
    """
    if op == 2:     # ==
        return same_ES_unit(this.unit, that.unit)
    elif op == 3:   # !=
        return not same_ES_unit(this.unit, that.unit)
    else:
        #return NotImplementedError
        raise TypeError, 'ESUnit only supports == and != comparisons'

cdef class ESUnit:
    """A Python class representing an ES unit.
    """

    # XXX Or would I be better of with an array.array (or, eventually, bytearray)?
    cdef ES_unit_p unit

    # It appears to be recommended to make __cinit__ expand to take more
    # arguments (if __init__ ever gains them), since both get the same
    # things passed to them. Hmm, normally I'd trust myself, but let's
    # try the recommended route
    def __cinit__(self, data=None, *args,**kwargs):
        cdef char       *buffer
        cdef Py_ssize_t  length
        if data:
            PyString_AsStringAndSize(data, &buffer, &length)
            retval = cwrapper.build_ES_unit_from_data(&self.unit, <byte *>buffer, length);
            if retval < 0:
                raise TSToolsException,'Error building ES unit from Python string'

    def __init__(self,data=None):
        pass

    def report(self):
        """Report (briefly) on an ES unit. This write to C stdout, which means
        that Python has no control over the output. A proper Python version of
        this will be provided eventually.
        """
        cwrapper.report_ES_unit(stdout, self.unit)

    def __dealloc__(self):
        cwrapper.free_ES_unit(&self.unit)

    def __str__(self):
        text = 'ES unit: start code %02x, len %4d:'%(self.unit.start_code,
                                                    self.unit.data_len)
        for 0 <= ii < min(self.unit.data_len,8):
            text += ' %02x'%self.unit.data[ii]

        if self.unit.data_len == 9:
            text += ' %02x'%self.unit.data[8]
        elif self.unit.data_len > 9:
            text += '...'
        return text

    def __repr__(self):
        return 'ESUnit("%s")'%_hexify_C_byte_array(self.unit.data,self.unit.data_len)

    cdef __set_es_unit(self, ES_unit_p unit):
        if self.unit == NULL:
            raise ValueError,'ES unit already defined'
        else:
            self.unit = unit

    def __richcmp__(self,other,op):
        return compare_ESUnits(self,other,op)

    def __getattr__(self,name):
        if name == 'start_posn':
            return ESOffset(self.unit.start_posn.infile,
                            self.unit.start_posn.inpacket)
        elif name == 'data':
            # Cast the first parameter so that the C compiler is happy
            # when compiling the (derived) tstools.c
            return PyString_FromStringAndSize(<char *>self.unit.data, self.unit.data_len)
        elif name == 'start_code':
            return self.unit.start_code
        elif name == 'PES_had_PTS':
            return self.unit.PES_had_PTS
        else:
            raise AttributeError

cdef class ESFile:
    """A Python class representing an ES stream.

    We support opening for read, or opening (creating) a new file
    for write. For the moment, we don't support appending, and
    support for trying to read and write the same file is undefined.

    So, create a new ESFile as either:

        * ESFile(filename,'r') or
        * ESFile(filename,'w')

    Note that there is always an implicit 'b' attached to the mode (i.e., the
    file is accessed in binary mode).
    """

    cdef FILE *file_stream      # The corresponding C file stream
    cdef int   fileno           # and file number
    cdef ES_p  stream           # For reading an existing ES stream
    cdef readonly object name
    cdef readonly object mode

    # It appears to be recommended to make __cinit__ expand to take more
    # arguments (if __init__ ever gains them), since both get the same
    # things passed to them. Hmm, normally I'd trust myself, but let's
    # try the recommended route
    def __cinit__(self,filename,mode='r',*args,**kwargs):
        self.file_stream = fopen(filename,mode)
        if self.file_stream == NULL:
            raise TSToolsException,"Error opening file '%s'"\
                    " with (actual) mode '%s': %s"%(filename,mode,strerror(errno))
        self.fileno = fileno(self.file_stream)
        if mode == 'r':
            retval = cwrapper.build_elementary_stream_file(self.fileno,&self.stream)
            if retval != 0:
                raise TSToolsException,'Error attaching elementary stream to file %s'%filename

    def __init__(self,filename,mode='r'):
        # What should go in __init__ and what in __cinit__ ???
        self.name = filename
        self.mode = mode

    def __dealloc__(self):
        if self.file_stream != NULL:
            retval = fclose(self.file_stream)
            if retval != 0:
                raise TSToolsException,"Error closing file '%s':"\
                        " %s"%(self.name,strerror(errno))
        if self.stream != NULL:
            cwrapper.free_elementary_stream(&self.stream)

    def __iter__(self):
        return self

    def __repr__(self):
        if self.name:
            if self.is_readable:
                return "<ESFile '%s' open for read>"%self.name
            else:
                return "<ESFile '%s' open for write>"%self.name
        else:
            return "<ESFile, closed>"

    def is_readable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'r' and self.stream != NULL

    def is_writable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'w' and self.file_stream != NULL

    cdef _next_ESUnit(self):
        cdef ES_unit_p unit
        # The C function assumes it has a valid ES stream passed to it
        # = I don't think we're always called with such
        if self.stream == NULL:
            raise TSToolsException,'No ES stream to read'

        retval = cwrapper.find_and_build_next_ES_unit(self.stream, &unit)
        if retval == EOF:
            raise StopIteration
        elif retval != 0:
            raise TSToolsException,'Error getting next ES unit from file %s'%self.name

        # From http://www.philhassey.com/blog/2007/12/05/pyrex-from-confusion-to-enlightenment/
        # Pyrex doesn't do type inference, so it doesn't detect that 'u' is allowed
        # to hold an ES_unit_p. It's up to us to *tell* it, specifically, what type
        # 'u' is going to be.
        cdef ESUnit u
        u = ESUnit()
        u.unit = unit
        return u

    # For Pyrex classes, we define a __next__ instead of a next method
    # in order to form our iterator
    def __next__(self):
        """Our iterator interface retrieves the ES units from the stream.
        """
        return self._next_ESUnit()

    def seek(self,*args):
        """Seek to the given 'offset', which should be the start of an ES unit.

        'offset' may be a single integer (if the file is a raw ES file), an
        ESOffset (for any sort of ES file), or a tuple of (infile,inpacket)

        Returns an ESOffset according to where it sought to.
        """
        cdef ES_offset where
        try:
            if len(args) == 1:
                try:
                    where.infile = args[0].infile
                    where.inpacket = args[0].inpacket
                except:
                    where.infile = args[0]
                    where.inpacket = 0
            elif len(args) == 2:
                where.infile, where.inpacket = args
            else:
                raise TypeError
        except:
            raise TypeError,'Seek argument must be one integer, two integers or an ESOffset'
        retval = cwrapper.seek_ES(self.stream,where)
        if retval != 0:
            raise TSToolsException,"Error seeking to %s in file '%s'"%(args,self.name)
        else:
            return ESOffset(where.infile,where.inpacket)

    def read(self):
        """Read the next ES unit from this stream.
        """
        try:
            return self._next_ESUnit()
        except StopIteration:
            raise EOFError

    def write(self, ESUnit unit):
        """Write an ES unit to this stream.
        """
        if self.file_stream == NULL:
            raise TSToolsException,'ESFile does not seem to have been opened for write'

        retval = cwrapper.write_ES_unit(self.file_stream,unit.unit)
        if retval != 0:
            raise TSToolsException,'Error writing ES unit to file %s'%self.name

    def close(self):
        # Apparently we can't call the __dealloc__ method itself,
        # but I think this is sensible to do here...
        if self.file_stream != NULL:
            retval = fclose(self.file_stream)
            if retval != 0:
                raise TSToolsException,"Error closing file '%s':"\
                        " %s"%(self.name,strerror(errno))
        if self.stream != NULL:
            cwrapper.free_elementary_stream(&self.stream)
        # And obviously we're not available any more
        self.file_stream = NULL
        self.fileno = -1
        self.name = None
        self.mode = None

    def __enter__(self):
        return self

    def __exit__(self, etype, value, tb):
        if tb is None:
            # No exception, so just finish normally
            self.close()
        else:
            # Exception occurred, so tidy up
            self.close()
            # And allow the exception to be re-raised
            return False

# =============================================================================
# TS matters
# =============================================================================

def _hexify_array(bytes):
    """Return a representation of an array of bytes as a hex values string.
    """
    words = []
    for val in bytes:
        words.append('\\x%02x'%val)
    return ''.join(words)

class PAT(object):
    """A Program Association Table.

    Always has PID 0x0000.

    Data is:

        * <to be defined>
        * dictionary of {program_number : pid}

    where the 'pid' is the relevant PMT pid.
    """

    def __init__(self, data=None):
        """Initialise the PAT, optionally with its dictionary.
        """
        self._data = {}
        if data:
            # Let our own setattr method check the items make sense
            for key,value in data.items():
                self[key] = value

    def __getitem__(self,key):
        return self._data[key]

    def __setitem__(self,key,value):
        if not (0 <= key <= 0xFFFF):
            raise ValueError,"Program number must be 0..65535, not %d"%key
        if not (0 <= value <= 0x1FFF):
            raise ValueError,"PID must be 0..0x1fff, not %#04x"%value
        self._data[key] = value

    def __delitem__(self,key):
        del self._data[key]

    def __len__(self):
        return len(self._data)

    def __contains__(self,key):
        return key in self._data

    def __eq__(self,other):
        return self._data == other._data

    def __iter__(self):
        return self._data.iteritems()

    def items(self):
        # Return the (program number, PMT PID) pairs from the PAT,
        # sorted by program number
        pairs = self._data.items()
        return sorted(pairs)

    def __repr__(self):
        """It is nicer if we make sure the dictionary appears in some sort of
        order.
        """
        words = []
        keys = self._data.keys()
        keys.sort()
        for key in keys:
            words.append('%d:%#x'%(key,self._data[key]))
        return 'PAT({%s})'%(','.join(words))

    def has_PMT(self,pid):
        """Return whether a particular PID belongs to a PMT.
        """
        return pid in self._data.values()

    def find_program_numbers(self,PMT_pid):
        """Given a PMT pid, return its program number(s), as a list.

        Note that technically one PID may be used in more than one program.

        Returns an empty list if the PID is not found
        """
        # XXX Is it worth maintaining an extra (reversed) dictionary instead?
        program_numbers = []
        for prog_num, pid in self._data():
            if pid == PMT_pid:
                program_numbers.append(prog_num)
        return program_numbers


# XXX Should this be an extension type, and enforce the datatypes it can hold?
# XXX Or is that just too much bother?
class ProgramStream(object):
    """A program stream, within a PMT.
    """

    def __init__(self,stream_type,elementary_PID,es_info):
        self.stream_type = stream_type
        self.elementary_PID = elementary_PID
        # Use an array for the same reasons discussed in TSPacket
        self.es_info = array.array('B',es_info)

    def __str__(self):
        """Return a fairly compact and (relatively) self-explanatory format
        """
        return "PID %04x (%4d) -> Stream type %02x (%3d) ES info '%s'"%(\
                                                            self.elementary_PID,
                                                            self.stream_type,
                                                            _hexify_array(self.es_info))


    def __repr__(self):
        """Return something we could be recreated from.
        """
        return "ProgramStream(%#02x,%#04x,'%s')"%(self.stream_type,
                                               self.elementary_PID,
                                               _hexify_array(self.es_info))

    def formatted(self):
        """Return a representation that is similar to that returned by the C tools.
        ...not easy for program streams
        """
        return self.__str__()

    def report(self,indent=2):
        print "%sPID %04x (%4d) -> Stream type %02x (%3d)"%(' '*indent,
                                                            self.elementary_PID,
                                                            self.elementary_PID,
                                                            self.stream_type,
                                                            self.stream_type)
        # XXX should actually output them as descriptors
        if self.es_info:
            print "%s    ES info '%s'"%(' '*indent,_hexify_array(self.es_info))

# XXX Should this be an extension type, and enforce the datatypes it can hold?
# XXX Or is that just too much bother?
class PMT(object):
    """A Program Map Table.

    Data is:

        * program_number, version_number, PCR_pid
        * program_info (bytes, as a "string")
        * a dictionary of the streams in this program, as:

            * key:   elementary_PID
            * value: (stream_type, ES_info) 
    """

    def __init__(self,program_number,version_number,PCR_pid):
        self.program_number = program_number
        self.version_number = version_number
        self.PCR_pid = PCR_pid

        # Use an array for the same reasons discussed in TSPacket
        self.program_info = array.array('B','')
        self.streams = []

    def set_program_info(self,program_info):
        """Set our program_info bytes.
        """
        self.program_info = array.array('B',program_info)

    def add_stream(self,stream):
        """Append a ProgramStream to our list of such.
        """
        # I *think* this is justified,
        # but I still suspect I shall come to regret it
        if not isinstance(stream,ProgramStream):
            raise TypeError('Argument to PMT.add_stream should be a ProgramStream')

        self.streams.append(stream)

    def __str__(self):
        # XXX Don't see what I can do aboout the program info and streams
        return "PMT program %d, version %d, PCR PID %04x (%d)"%(self.program_number,
                                                                self.version_number,
                                                                self.PCR_pid,
                                                                self.PCR_pid)

    def __repr__(self):
        # XXX Don't see what I can do aboout the program streams
        return "PMT(%d,%d,%#04x,'%s')"%(self.program_number,
                                        self.version_number,
                                        self.PCR_pid,
                                        _hexify_array(self.program_info))

    def formatted(self):
        """Return a representation that is similar to that returned by the C tools.
        ...not easy for PMT
        """
        return self.__str__()

    def report(self):
        print "PMT program %d, version %d, PCR PID %04x (%d)"%(self.program_number,
                                                               self.version_number,
                                                               self.PCR_pid,
                                                               self.PCR_pid)
        # XXX should actually output them as descriptors
        if self.program_info:
            print "  Program info '%s'"%_hexify_array(self.program_info)
        if self.streams:
            print "  Program streams:"
            for stream in self.streams:
                stream.report(indent=4)


DEF TS_PACKET_LEN = 188

cdef class TSPacket:
    """A convenient representation of a (dissected) TS packet.
    """

    cdef readonly object    data
    cdef readonly PID       pid

    # The following are lazily calculated if necessary
    cdef  byte      _already_split
    cdef  int       _pusi       # payload unit start indicator
    cdef  object    _adapt
    cdef  object    _payload

    # Ditto with looking for a PCR
    cdef  int       _checked_for_pcr
    cdef  object    _pcr        # if we have one

    def __cinit__(self,buffer,*args,**kwargs):
        """The buffer *must* be 188 bytes long, by definition.
        """
        # An array is easier to access than a string, and can be initialised
        # from any sensible sequence. This may not be the most efficient thing
        # to do, though, so later on we might want to consider ways of iterating
        # over TS entries in a file without needing to create TS packets...
        self.data = array.array('B',buffer)
        # We *really* believe that the first character had better be 0x47...
        if self.data[0] != 0x47:
            raise TSToolsException,\
                    'First byte of TS packet is %#02x, not 0x47'%(ord(buffer[0]))
        # And the length is, well, defined
        if len(self.data) != TS_PACKET_LEN:
            raise TSToolsException,\
                    'TS packet is %d bytes long, not %d'%(len(self.data)) 
        # The PID is useful to know early on, and fairly easy to work out
        self.pid = ((ord(buffer[1]) & 0x1F) << 8) | ord(buffer[2])

    def __init__(self,pid=None,pusi=None,adapt=None,payload=None,data=None):
        pass

    def __dealloc__(self):
        pass

    def is_padding(self):
        return self.pid == 0x1fff

    def __str__(self):
        self._split()
        text = 'TS packet PID %04x '%self.pid
        if self.pusi:
            text += '[pusi] '
        if self.adapt and self.payload:
            text += 'A+P '
        elif self.adapt:
            text += 'A '
        elif self.payload:
            text += 'P '
        data = self.data[3:11]
        words = []
        for val in data:
            words.append('%02x'%val)
        text += ' '.join(words) + '...'
        return text

    def __repr__(self):
        return 'TSPacket("%s")'%_hexify_array(self.data)

    def __richcmp__(self,other,op):
        if op == 2:     # ==
            return self.data == other.data
        elif op == 3:   # !=
            return self.data != other.data
        else:
            #return NotImplementedError
            raise TypeError, 'TSPacket only supports == and != comparisons'

    def _split(self):
        """Split the packet up when requested to do so.
        """
        cdef const_void_ptr buffer
        cdef Py_ssize_t     length
        cdef PID            pid
        cdef char          *adapt_buf
        cdef int            adapt_len
        cdef char          *payload_buf
        cdef int            payload_len
        cdef int            retval
        PyObject_AsReadBuffer(self.data, &buffer, &length)
        retval = cwrapper.split_TS_packet(<byte *>buffer,&pid,&self._pusi,
                                 <byte **>&adapt_buf,&adapt_len,
                                 <byte **>&payload_buf,&payload_len)
        if retval != 0:
            raise TSToolsException,'Error splitting TS packet data'
        if adapt_len == 0:
            self._adapt = None
        else:
            self._adapt = PyString_FromStringAndSize(adapt_buf,adapt_len)
        if payload_len == 0:
            self._payload = None
        else:
            self._payload = PyString_FromStringAndSize(payload_buf,payload_len)
        self._already_split = True

    def _determine_PCR(self):
        """Determine our PCR, if we have one.
        Assumes that self._split() has been called already.
        """
        cdef const_void_ptr adapt_buf
        cdef Py_ssize_t     adapt_len
        cdef int            got_pcr
        cdef uint64_t       pcr
        if self._adapt:
            PyObject_AsReadBuffer(self._adapt, &adapt_buf, &adapt_len)
            cwrapper.get_PCR_from_adaptation_field(<byte *>adapt_buf, adapt_len,
                                          &got_pcr, &pcr)
        else:
            got_pcr = 0
        self._checked_for_pcr = True    # regardless
        if got_pcr:
            self._pcr = pcr

    def __getattr__(self,name):
        if not self._already_split:
            self._split()
        if name == 'pusi':
            return self._pusi
        elif name == 'adapt':
            return self._adapt
        elif name == 'payload':
            return self._payload
        elif name == "PCR":
            if not self._checked_for_pcr:
                self._determine_PCR()
            return self._pcr
        else:
            raise AttributeError

cdef pat_from_prog_list(pidint_list_p prog_list):
    """Convert a program list into a PAT instance.
    """
    try:
        pat = PAT()
        for 0 <= ii < prog_list.length:
            pat[prog_list.number[ii]] = prog_list.pid[ii]
        return pat
    finally:
        cwrapper.free_pidint_list(&prog_list)

cdef pmt_from_pmt_p(pmt_p pmt):
    """Convert a C PMT structure into a PMT instance.
    XXX Should we remember the PMT's PID?

    Returns the new PMT object, or None if none
    """
    try:
        this = PMT(pmt.program_number,
                   pmt.version_number,
                   pmt.PCR_pid)

        prog_info = PyString_FromStringAndSize(<char *>pmt.program_info,
                                               pmt.program_info_length)
        this.set_program_info(prog_info)

        for 0 <= ii < pmt.num_streams:
            es_info = PyString_FromStringAndSize(<char *>pmt.streams[ii].ES_info,
                                                 pmt.streams[ii].ES_info_length)
            stream = ProgramStream(pmt.streams[ii].stream_type,
                                   pmt.streams[ii].elementary_PID,
                                   es_info)

            this.add_stream(stream)
        return this
    finally:
        cwrapper.free_pmt(&pmt)

cdef class _PAT_accumulator:
    """This is just an accumulator for a single PAT's data.
    """
    cdef byte *pat_data
    cdef int   pat_data_len
    cdef int   pat_data_used

    def __cinit__(self):
        pass

    def __init__(self):
        pass

    def __dealloc__(self):
        self.clear()

    def clear(self):
        """Clear our internal buffers
        """
        if self.pat_data != NULL:
            free(<void *>self.pat_data)
        self.pat_data = NULL
        self.pat_data_len = self.pat_data_used = 0

    def started(self):
        """Have we started accumulating data?
        """
        return self.pat_data != NULL

    cdef accumulate(self, byte *payload_buf, int payload_len):
        """Add a bit more to our accumulating data.
        """
        cdef int retval
        retval =  cwrapper.build_psi_data(False,payload_buf,payload_len,0,
                                 &self.pat_data,&self.pat_data_len,
                                 &self.pat_data_used)
        return retval

    def finished(self):
        """Have we all the data we need for our PAT?
        """
        return self.pat_data_len == self.pat_data_used

    cdef extract(self):
        """Finally extract an actual PAT from the accumulated data.
        """
        cdef pidint_list_p  prog_list
        cdef int            retval
        retval = cwrapper.extract_prog_list_from_pat(False,
                                            self.pat_data,self.pat_data_len,
                                            &prog_list)
        if retval:
            raise TSToolsException,'Error extracting program list from PAT'
        return pat_from_prog_list(prog_list)

cdef class _PMT_accumulator:
    """This is just an accumulator for a single PMT's data.
    """
    cdef PID   pid
    cdef byte *pmt_data
    cdef int   pmt_data_len
    cdef int   pmt_data_used

    def __cinit__(self, pid):
        self.pid = pid

    def __init__(self, pid):
        pass

    def __dealloc__(self):
        self.clear()

    def clear(self):
        """Clear our internal buffers
        """
        if self.pmt_data != NULL:
            free(<void *>self.pmt_data)
        self.pmt_data = NULL
        self.pmt_data_len = self.pmt_data_used = 0

    cdef accumulate(self, byte *payload_buf, int payload_len):
        """Add a bit more to our accumulating data.
        """
        cdef int retval
        retval =  cwrapper.build_psi_data(False,payload_buf,payload_len,self.pid,
                                 &self.pmt_data,&self.pmt_data_len,
                                 &self.pmt_data_used)
        return retval

    def finished(self):
        """Have we all the data we need for our PMT?
        """
        return self.pmt_data_len == self.pmt_data_used

    cdef extract(self):
        """Finally extract an actual PMT from the accumulated data.
        """
        cdef pmt_p  pmt
        cdef int    retval
        retval = cwrapper.extract_pmt(False, self.pmt_data, self.pmt_data_len,
                             self.pid, &pmt)
        if retval:
            raise TSToolsException,'Error extracting PMT'
        return pmt_from_pmt_p(pmt)

cdef class TSFile:
    """A Python class representing a TS file.

    We support opening for read, or opening (creating) a new file
    for write. For the moment, we don't support appending, and
    support for trying to read and write the same file is undefined.

    So, create a new TSFile as either:

        * TSFile(filename,'r') or
        * TSFile(filename,'w')

    Note that there is always an implicit 'b' attached to the mode (i.e., the
    file is accessed in binary mode).

    When reading, the default is to read with "PCR buffering" enabled.
    
    If "PCR buffering" is enabled, then we always read-ahead enough so that we
    have two PCRs in hand -- the previous and the next. This allows us to
    assign an exact PCR value to every TS packet.

    If "PCR buffering" is not enabled, then we only know PCR values for those
    TS packets that actually contain an explicit PCR.
    """

    cdef TS_reader_p    tsreader

    cdef readonly object name
    cdef readonly object mode

    cdef readonly object PAT        # The latest PAT read, if any
    cdef readonly object PMT        # A dictionary of {program number : PMT}

    # We have a byte buffer in which we accumulate partial PAT parts,
    # as we read TS packets
    cdef _PAT_accumulator PAT_data

    # We have a dictionary linking PMT PID to each individual accumulator
    # for PMT data
    cdef object PMT_data

    # It appears to be recommended to make __cinit__ expand to take more
    # arguments (if __init__ ever gains them), since both get the same
    # things passed to them. Hmm, normally I'd trust myself, but let's
    # try the recommended route
    def __cinit__(self,filename,*args,**kwargs):
        pass

    def __init__(self,filename,mode='r'):
        # In practice, we need to do the actual opening of the file here,
        # because we wish to subclassable by BufferedTSFile, which only
        # supports mode 'r' for its files.
        # However, as the Pyrex documentation warns that our __init__
        # method *might* get called more than once, don't try to open
        # a file more than once...

        if self.tsreader:       # Oh dear, we're already open
            if filename != self.filename or mode != self.mode:
                raise TSToolsException,"Attempt to reopen %s as '%s' with mode '%s'"%\
                        (self.__repr__,filename,mode)
            return

        self.name = filename
        self.mode = mode
        self.PMT = {}
        self.PAT_data = _PAT_accumulator()
        self.PMT_data = {}

        if mode == 'r':
            retval = cwrapper.open_file_for_TS_read(filename,&self.tsreader)
            if retval == 1:
                raise TSToolsException,"Error opening file '%s'"\
                        " for TS reading: %s"%(filename,strerror(errno))
        elif mode == 'w':
            raise NotImplementedError,"TSFile mode 'w' is not yet available"
        else:
            raise TSToolsException,"Error opening file '%s'"\
                    " with mode '%s' (only 'r' and 'w' supported)"%(filename,mode)

    def _clear_pat_data(self):
        """Clear the buffers we use to accumulate PAT data
        (but not any actual PAT we have acquired).
        """
        if self.PAT_data:
            self.PAT_data.clear()

    def _clear_pmt_data(self,pid):
        """Clear the buffers we use to accunulate PMT data
        (but not any actual PMT we have acquired).
        """
        if pid in self.PMT_data:
            self.PMT_data[pid].clear()
            del self.PMT_data[pid]

    def _clear_all_pmt_data(self):
        """Clear the PMT accumulating buffers for all PIDs.
        """
        for pid in self.PMT_data:
            self.PMT_data[pid].clear()
        self.PMT_data = {}

    # (__dealloc__ is apparently not allowed to call Python methods,
    # and Python methods don't seem to be allowed to call __dealloc__,
    # so let's have an intermediary)
    cdef _close_for_read(self):
        if self.tsreader != NULL:
            self._clear_pat_data()
            self._clear_all_pmt_data()
            self.PAT = None
            self.PMT = None
            retval = cwrapper.close_TS_reader(&self.tsreader)
            if retval != 0:
                raise TSToolsException,"Error closing file '%s':"\
                        " %s"%(self.name,strerror(errno))

    def __dealloc__(self):
        self._close_for_read()
        #if self.tsreader != NULL:
        #    retval = close_TS_reader(&self.tsreader)
        #    if retval != 0:
        #        raise TSToolsException,"Error closing file '%s':"\
        #                " %s"%(self.name,strerror(errno))

    def __iter__(self):
        return self

    def __repr__(self):
        if self.name:
            if self.is_readable:
                return "<TSFile '%s' open for read>"%self.name
            else:
                return "<TSFile '%s' open for write>"%self.name
        else:
            return "<TSFile, closed>"

    def is_readable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'r' and self.tsreader != NULL
        pass

    def is_writable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'w'
        #return self.mode == 'w' and self.file_stream != NULL
        pass

    cdef _check_pat_pmt(self, byte *buffer):
        cdef PID         pid
        cdef int         pusi
        cdef byte       *adapt_buf
        cdef int         adapt_len
        cdef byte       *payload_buf
        cdef int         payload_len
        cdef int         retval
        retval = cwrapper.split_TS_packet(buffer, &pid, &pusi,
                                 &adapt_buf,&adapt_len,
                                 &payload_buf,&payload_len)
        if retval != 0:
            # We couldn't split it up - presumably a broken TS packet.
            # Ignore this problem, as the caller might legitimately want
            # to retrieve broken TS packets and inspect them, and our wish
            # to find (parts of) PAT packets shouldn't make that harder
            return
        if pid == 0:
            self._check_pat(pusi,adapt_buf,adapt_len,payload_buf,payload_len)
        else:
            self._check_pmt(pid,pusi,adapt_buf,adapt_len,payload_buf,payload_len)

    cdef _check_pat(self, int pusi, byte *adapt_buf, int adapt_len,
                    byte *payload_buf, int payload_len):
        """Check if the current buffer represents (another) part of a PAT
        """
        # Methodology borrowed from tsreport.c::report_ts
        cdef int retval
        cdef pidint_list_p  prog_list
        cdef _PAT_accumulator this_pat_data

        if pusi:
            if self.PAT_data.started():
                # Lose the PAT data we'd already partially accumulated
                # XXX should we grumble out loud at this? Probably not here,
                # XXX although note that the equivalent C code might
                self._clear_pat_data()
        else:
            if not self.PAT_data.started():
                # It's not the start of a PAT, and we haven't got a PAT
                # to continue, so the best we can do is ignore it
                # XXX again, for the moment, quietly
                return

        # Otherwise, call the "accumulate bits of a PAT" function,
        # which does most of the heavy lifting for us
        retval = self.PAT_data.accumulate(payload_buf,payload_len)
        if retval:
            # For the moment, just give up
            self._clear_pat_data()
            return

        if self.PAT_data.finished():
            # We've got it all
            try:
                self.PAT = self.PAT_data.extract()
            finally:
                self._clear_pat_data()

    cdef _check_pmt(self, PID pid,
                    int pusi, byte *adapt_buf, int adapt_len,
                    byte *payload_buf, int payload_len):
        """Check if the current buffer represents (another) part of a PMT
        """
        # Methodology borrowed from tsreport.c::report_ts
        cdef int         retval
        cdef _PMT_accumulator  this_pmt_data
        cdef pmt_p       pmt_ptr

        # We can't tell if this is a PMT until we've had a PAT, so:
        if self.PAT is None:
            return

        # So, are we actually a PMT?
        if not self.PAT.has_PMT(pid):
            return

        # Note that whilst we support a PMT PID belonging to more than
        # one program, we don't support interleaving of parts of such
        # - i.e., once a PMT with a given PID has started, we assume
        # that all the partial PMT records with the same PID belong
        # together...

        if pusi:
            if pid in self.PMT_data:
                # Lose the PMT data we'd already partially accumulated for
                # this PMT PID
                # XXX should we grumble out loud at this? Probably not here,
                # XXX although note that the equivalent C code might
                self._clear_pmt_data(pid)
            this_pmt_data = self.PMT_data[pid] = _PMT_accumulator(pid)
        else:
            if pid in self.PMT_data:
                this_pmt_data = self.PMT_data[pid]
            else:
                # It's not the start of a PMT, and we haven't got a PMT
                # to continue, so the best we can do is ignore it
                # XXX again, for the moment, quietly
                return

        # Otherwise, call the "accumulate bits of a PMT" function,
        # which does most of the heavy lifting for us
        retval = this_pmt_data.accumulate(payload_buf,payload_len)
        if retval:
            # For the moment, just give up
            self._clear_pmt_data(pid)
            return

        if this_pmt_data.finished():
            # We've got it all
            try:
                # Finally, our PMT
                pmt = this_pmt_data.extract()
                # And remember it on the file as well
                self.PMT[pmt.program_number] = pmt
            finally:
                self._clear_pmt_data(pid)

    cdef TSPacket _next_TSPacket(self):
        """Read the next TS packet and return an equivalent TSPacket instance.

        ``filename`` is given for use in exception messages - it should be the
        name of the file we're reading from (using ``tsreader``).
        """
        cdef byte *buffer
        if self.tsreader == NULL:
            raise TSToolsException,'No TS stream to read'
        retval = cwrapper.read_next_TS_packet(self.tsreader, &buffer)
        if retval == EOF:
            raise StopIteration
        elif retval == 1:
            raise TSToolsException,'Error getting next TS packet from file %s'%self.name

        # Remember the buffer we get handed a pointer to is transient
        # so we need to take a copy of it (which we might as well keep in
        # a Python object...)
        buffer_str = PyString_FromStringAndSize(<char *>buffer, TS_PACKET_LEN)
        try:
            new_packet = TSPacket(buffer_str)
        except TSToolsException, what:
            raise TSToolsException,\
                    'Error getting next TS packet from file %s (%s)'%(self.name,what)

        # Check whether this packet updates our idea of the current PAT
        # or PMT
        #
        # (We call this *after* calling TSPacket, becuse if we call it first
        # then, for instance, TSPacket('\0xff') would cause split_TS_packet,
        # within _check_pat, to output errors on C stderr, followed by TSPacket
        # detecting the problem anyway)
        self._check_pat_pmt(buffer)

        return new_packet

    # For Pyrex classes, we define a __next__ instead of a next method
    # in order to form our iterator
    def __next__(self):
        """Our iterator interface retrieves the TS packets from the stream.
        """
        return self._next_TSPacket()

    def seek(self,offset):
        """Seek to the given offset, which should be a multiple of 188.

        Note that the method does not check the value of 'offset'.

        Seeking causes the file to "forget" any PAT data it may have deduced
        from sequential reading of the file, or by explicit calls of find_PAT.
        """
        self._clear_pat_data
        self.PAT = None
        retval = cwrapper.seek_using_TS_reader(self.tsreader,offset)
        if retval == 1:
            raise TSToolsException,'Error seeking to %d in file %s'%(offset,self.name)

    def read(self):
        """Read the next TS packet from this stream.
        """
        try:
            return self._next_TSPacket()
        except StopIteration:
            raise EOFError

    def write(self, TSPacket tspacket):
        """Write a TS packet to this stream.
        """
        pass

    def find_PAT(self,max=0,verbose=False,quiet=False):
        """Read TS packets to find the (next) PAT.

        If non-zero, `max` is the maximum number of TS packets to scan forwards
        whilst looking. If it is zero, there is no limit.

        If `verbose` is True, then extra information is output. If `quiet` is
        True, then the search will be as quiet as possible.

        Returns (num_read, pat), where `num_read` is how many TS packets were
        read (whether the PAT is found or not), and `pat` is None if no PAT
        was found.

        The new PAT is also saved as self.PAT (replacing, rather than updating,
        any previous self.PAT object).

        This method is more efficient than using repeated calls of ``read``,
        because it uses the underlying C function to find the next PAT.
        """
        cdef pidint_list_p  prog_list
        cdef int            num_read
        if self.tsreader == NULL:
            raise TSToolsException,'No TS stream to read'
        retval = cwrapper.find_pat(self.tsreader,max,verbose,quiet,&num_read,&prog_list)
        if retval == EOF:       # No PAT found
            return (num_read,None)
        elif retval == 1:
            raise TSToolsException,'Error searching for next PAT'
        # Don't forget to remember it on the file as well
        self.PAT = pat_from_prog_list(prog_list)
        return (num_read,self.PAT)

    def find_PMT(self,pmt_pid,program_number=-1,max=0,verbose=False,quiet=False):
        """Read TS packets to find the (next) PMT with PID `pmt_pid`.

        If `program_number` is 0 or more, then only a PMT with that program
        number will do, otherwise any PMT of the given PID will be OK.

        If non-zero, `max` is the maximum number of TS packets to scan forwards
        whilst looking. If it is zero, there is no limit.

        If `verbose` is True, then extra information is output. If `quiet` is
        True, then the search will be as quiet as possible.

        Returns (num_read, pmt), where `num_read` is how many TS packets were
        read (whether the PMT is found or not), and `pmt` is None if no
        appropriate PMT was found.

        The new PMT is also saved as self.PMT[progno] (replacing, rather than
        updating, any previous self.PMT[progno] object), where `progno` is the
        actual program number of the PMT.

        This method is more efficient than using repeated calls of ``read``,
        because it uses the underlying C function to find the next PMT.
        """
        cdef pmt_p     pmt
        cdef int       num_read
        cdef unsigned  actual_prog_num
        if self.tsreader == NULL:
            raise TSToolsException,'No TS stream to read'
        retval = cwrapper.find_next_pmt(self.tsreader,pmt_pid,program_number,max,verbose,quiet,
                               &num_read,&pmt)
        if retval == EOF:       # No PMT found
            return (num_read,None)
        elif retval == 1:
            raise TSToolsException,'Error searching for next PMT'
        this_pmt = pmt_from_pmt_p(pmt)

        # Don't forget to remember it on the file as well
        self.PMT[this_pmt.program_number] = this_pmt

        return (num_read,this_pmt)

    def close(self):
        ## Since we don't appear to be able to call our __dealloc__ "method",
        ## and we're not allowed to call Python methods..
        #if self.tsreader != NULL:
        #    retval = close_TS_reader(&self.tsreader)
        #    if retval != 0:
        #        raise TSToolsException,"Error closing file '%s':"\
        #                " %s"%(self.name,strerror(errno))
        self._close_for_read()
        self.name = None
        self.mode = None

    def __enter__(self):
        return self

    def __exit__(self, etype, value, tb):
        if tb is None:
            # No exception, so just finish normally
            self.close()
        else:
            # Exception occurred, so tidy up
            self.close()
            # And allow the exception to be re-raised
            return False

cdef class BufferedTSFile(TSFile):
    """A Python class representing a PCR-buffered TS file.

    This provides a read-only TSFile in which all TS packets have a reliable
    PCR. This is managed by:

        1. Locating the first PAT.
        2. Locating the first PMT associated with that PAT
        3. Reading TS packets until two PMTs have been found with a PCR.
        4. Deducing the PCR values for intermediate TS packets based on
           those PCRs and the locations of the PMT packets within the
           file.
        5. "Rewinding" back to the first PMT to beging reading packets.

    Note that this last means the first packets of the file are likely to be
    ignored, which is a bug, and should eventually be fixed.

    Further note that the current implementation doesn't offer any means of
    changing which PMT PID is used, which program is selected, etc -- the PMT
    from the first program stream in the first PAT will be the one chosen.
    """

    cdef object    got_first   # Have we already read the first TS packet?
    cdef object    pcr_pid     # The PID we're using for our PCRs
    cdef uint32_t  start_count # A hack

    # The __cinit__ of our base type (TSFile) is automatically called
    # for us, before our own __cinit__
    def __cinit__(self,filename,*args,**kwargs):
        pass

    def __init__(self,filename):
        """Open the given file for reading via the PCR buffering mechanism.
        """
        super(BufferedTSFile,self).__init__(filename,mode='r')

        # Locate our first PMT
        (num_read,PAT) = self.find_PAT()
        if PAT is None:
            raise TSToolsException,"Unable to find PAT in file '%s'"%self.name

        self.start_count = num_read

        # Choose the first program from therein (the list returned is sorted
        # by program number)
        programs = PAT.items()
        if len(programs) == 0:
            raise TSToolsException,"No programs in first PAT in '%s'"%self.name

        # Find the PMT for the first program
        (progno,PMT_pid) = programs[0]
        (num_read,PMT) = self.find_PMT(PMT_pid,progno)
        if PMT is None:
            raise TSToolsException,"Unable to find PMT with PID %04x"\
                    " for program %d in file '%s'"%(PMT_pid,progno,self.name)

        self.start_count += num_read
        self.pcr_pid      = PMT.PCR_pid

        # Tell the read mechanism which PCR PID we want to use
        retval = cwrapper.prime_read_buffered_TS_packet(self.tsreader,self.pcr_pid)
        if retval == 1:
            raise TSToolsException,'Error priming PCR read ahead for file %s'%self.name


    def __repr__(self):
        if self.name:
            return "<BufferedTSFile '%s' open for read>"%self.name
        else:
            return "<BufferedTSFile, closed>"

    def write(self, TSPacket tspacket):
        """BufferedTSFiles do not support writing.
        """
        raise NotImplementedError,'BufferedTSFiles do not support writing'

    cdef TSPacket _next_TSPacket(self):
        """Read the next TS packet and return an equivalent TSPacket instance.

        ``filename`` is given for use in exception messages - it should be the
        name of the file we're reading from (using ``tsreader``).
        """
        cdef byte     *buffer
        cdef PID       pid
        cdef uint64_t  pcr
        cdef uint32_t  count
        if self.tsreader == NULL:
            raise TSToolsException,'No TS stream to read'

        if self.got_first:
            retval = cwrapper.read_next_TS_packet_from_buffer(self.tsreader, &buffer,
                                                     &pid, &pcr)
        else:
            retval = cwrapper.read_first_TS_packet_from_buffer(self.tsreader, self.pcr_pid,
                                                      self.start_count,
                                                      &buffer,
                                                      &pid, &pcr, &count)
        if retval == EOF:
            raise StopIteration
        elif retval == 1:
            raise TSToolsException,'Error getting next TS packet from file %s'%self.name

        self.got_first = True

        # Remember the buffer we get handed a pointer to is transient
        # so we need to take a copy of it (which we might as well keep in
        # a Python object...)
        buffer_str = PyString_FromStringAndSize(<char *>buffer, TS_PACKET_LEN)
        try:
            # XXX And we really must tell the TSPacket that we *know* its PCR
            new_packet = TSPacket(buffer_str)
        except TSToolsException, what:
            raise TSToolsException,\
                    'Error getting next TS packet from file %s (%s)'%(self.name,what)

        # Check whether this packet updates our idea of the current PAT
        # or PMT
        #
        # (We call this *after* calling TSPacket, becuse if we call it first
        # then, for instance, TSPacket('\0xff') would cause split_TS_packet,
        # within _check_pat, to output errors on C stderr, followed by TSPacket
        # detecting the problem anyway)
        self._check_pat_pmt(buffer)

        return new_packet

# ----------------------------------------------------------------------
# vim: set filetype=python expandtab shiftwidth=4:
# [X]Emacs local variables declaration - place us into python mode
# Local Variables:
# mode:python
# py-indent-offset:4
# End:
