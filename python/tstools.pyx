"""tstools.pyx -- Pyrex bindings for the TS tools

This is being developed on a Mac, running OS X. I expect that it will also
build on a Linux machine. I do not expect it to build (as it stands) on
Windows, as it is making assumptions that may not follow thereon.

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

# If we're going to use definitions like this in more than one pyx file, we'll
# need to define the shared types in a .pxd file and use cimport to import
# them.

cdef extern from "stdio.h":
    ctypedef struct FILE:
        int _fileno
    cdef enum:
        EOF = -1
    cdef FILE *stdout
    # Associate a stream (returned) with an existing file descriptor.
    # The specified mode must be compatible with the existing mode of
    # the file descriptor. Closing the stream will close the descriptor
    # as well.
    cdef FILE *fdopen(int fildes, char *mode)

    cdef FILE *fopen(char *path, char *mode)
    cdef int fclose(FILE *stream)
    cdef int fileno(FILE *stream)

cdef extern from "errno.h":
    cdef int errno

cdef extern from "string.h":
    cdef char *strerror(int errnum)

# Copied from the Pyrex documentation...
cdef extern from "Python.h":
    # Return a new string object with a copy of the string v as value and
    # length len on success, and NULL on failure. If v is NULL, the contents of
    # the string are uninitialized.
    object PyString_FromStringAndSize(char *v, int len)

    # Return a NUL-terminated representation of the contents of the object obj
    # through the output variables buffer and length. 
    #
    # The function accepts both string and Unicode objects as input. For
    # Unicode objects it returns the default encoded version of the object. If
    # length is NULL, the resulting buffer may not contain NUL characters; if
    # it does, the function returns -1 and a TypeError is raised. 
    #
    # The buffer refers to an internal string buffer of obj, not a copy. The
    # data must not be modified in any way, unless the string was just created
    # using PyString_FromStringAndSize(NULL, size). It must not be deallocated.
    # If string is a Unicode object, this function computes the default
    # encoding of string and operates on that. If string is not a string object
    # at all, PyString_AsStringAndSize() returns -1 and raises TypeError.
    int PyString_AsStringAndSize(object obj, char **buffer, Py_ssize_t* length) except -1

cdef FILE *convert_python_file(object file):
    """Given a Python file object, return an equivalent stream.
    There are *so many things* dodgy about doing this...
    """
    cdef int fileno
    cdef char *mode
    cdef FILE *stream
    fileno = file.fileno()
    mode = file.mode
    stream = fdopen(fileno, mode) 
    if stream == NULL:
        raise TSToolsException, 'Error converting Python file to C FILE *'
    else:
        return stream

cdef extern from "stdint.h":
    ctypedef int uint8_t        # !!! *some* sort of int..
    ctypedef int uint32_t       # !!! *some* sort of int..

# PIDs are too long for 16 bits, short enough to fit in 32
ctypedef uint32_t   PID

cdef extern from "compat.h":
    # We don't need to define 'offset_t' exactly, just to let Pyrex
    # know it's vaguely int-like
    ctypedef int offset_t
    ctypedef uint8_t byte

cdef extern from 'es_defns.h':
    # The reader for an ES file
    struct elementary_stream:
        pass

    ctypedef elementary_stream  ES
    ctypedef elementary_stream *ES_p

    # A location within said stream
    struct _ES_offset:
        offset_t    infile      # as used by lseek
        int         inpacket    # in PES file, offset within PES packet

    ctypedef _ES_offset ES_offset

    # An actual ES unit
    struct ES_unit:
        ES_offset   start_posn
        byte       *data
        unsigned    data_len
        unsigned    data_size
        byte        start_code
        byte        PES_had_PTS

    ctypedef ES_unit *ES_unit_p

cdef extern from 'es_fns.h':
    int open_elementary_stream(char *filename, ES_p *es)
    void close_elementary_stream(ES_p *es)

    int build_elementary_stream_file(int input, ES_p *es)
    void free_elementary_stream(ES_p *es)

    int find_and_build_next_ES_unit(ES_p es, ES_unit_p *unit)
    void free_ES_unit(ES_unit_p *unit)
    void report_ES_unit(FILE *stream, ES_unit_p unit)

    # We perhaps need a Python object to represent an ES_offset?
    # Otherwise, it's going to be hard to use them within Python itself
    int seek_ES(ES_p es, ES_offset where)
    int compare_ES_offsets(ES_offset offset1, ES_offset offset2)

    # I'd like to be able to *write* ES files, so...
    # Python file objects can return a file descriptor (i.e., integer)
    # via their fileno() method, so the simplest thing to do may be to
    # add a new C function that uses write() instead of fwrite(). Or I
    # could use fdopen to turn the fileno() into a FILE *...
    int build_ES_unit_from_data(ES_unit_p *unit, byte *data, unsigned data_len)
    int write_ES_unit(FILE *output, ES_unit_p unit)


# Is this the best thing to do?
class TSToolsException(Exception):
    pass

cdef same_ES_unit(ES_unit_p this, ES_unit_p that):
    """Two ES units do not need to be at the same place to be the same.
    """
    if this.data_len != that.data_len:
        return False
    for 0 <= ii < this.data_len:
        if this.data[ii] != that.data[ii]:
            return False
    return True

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

    def __repr__(self):
        """Return a fairly compact and (relatively) self-explanatory format
        """
        return '%d+%d'%(self.infile,self.inpacket)

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

cdef class ESUnit       # Forward declaration
cdef object compare_ESUnits(ESUnit this, ESUnit that, int op):
    """op is 2 for ==, 3 for !=, other values not allowed.
    """
    if op == 2:     # ==
        return same_ES_unit(this.unit, that.unit)
    elif op == 3:   # !=
        return not same_ES_unit(this.unit, that.unit)
    else:
        #return NotImplemented
        raise TypeError, 'ESUnit only supports == and != comparisons'

cdef class ESUnit:
    """A Python class representing an ES unit.
    """

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
            retval = build_ES_unit_from_data(&self.unit, <byte *>buffer, length);
            if retval < 0:
                raise TSToolsException,'Error building ES unit from Python string'

    def __init__(self,data=None):
        pass

    def report(self):
        """Report (briefly) on an ES unit. This write to C stdout, which means
        that Python has no control over the output. A proper Python version of
        this will be provided eventually.
        """
        report_ES_unit(stdout, self.unit)

    def __dealloc__(self):
        free_ES_unit(&self.unit)

    def __repr__(self):
        text = 'ES unit: start code %02x, len %4d:'%(self.unit.start_code,
                                                    self.unit.data_len)
        for 0 <= ii < min(self.unit.data_len,8):
            text += ' %02x'%self.unit.data[ii]

        if self.unit.data_len == 9:
            text += ' %02x'%self.unit.data[8]
        elif self.unit.data_len > 9:
            text += '...'
        return text

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

# Is this the simplest way? Since it appears that a class method
# doesn't want to take a non-Python item as an argument...
cdef _next_ESUnit(ES_p stream, filename):
    cdef ES_unit_p unit
    # The C function assumes it has a valid ES stream passed to it
    # = I don't think we're always called with such
    if stream == NULL:
        raise TSToolsException,'No ES stream to read'

    retval = find_and_build_next_ES_unit(stream, &unit)
    if retval == EOF:
        raise StopIteration
    elif retval != 0:
        raise TSToolsException,'Error getting next ES unit from file %s'%filename

    # I'd like to be able to do:
    #     return ESUnit(unit=unit)
    # but it's not possible to pass anything other than a Python object
    # to methods, and an ES_unit_p is not (which is the point of what we're
    # doing!). I could take the innards of the ES unit, and pass them as
    # individual arguments, appropriately mangled, but that seems a bit like
    # overkill when the (rather inelegant but at least hidden in this factory
    # method) approach below actually works.
    # Maybe I'll figure out something better as I learn more about Pyrex.

    # From http://www.philhassey.com/blog/2007/12/05/pyrex-from-confusion-to-enlightenment/
    # Pyrex doesn't do type inference, so it doesn't detect that 'u' is allowed
    # to hold an ES_unit_p. It's up to us to *tell* it, specifically, what type
    # 'u' is going to be.
    cdef ESUnit u
    u = ESUnit()
    u.unit = unit
    return u

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
        actual_mode = mode+'b'
        self.file_stream = fopen(filename,mode)
        if self.file_stream == NULL:
            raise TSToolsException,"Error opening file '%s'"\
                    " with (actual) mode '%s': %s"%(filename,mode,strerror(errno))
        self.fileno = fileno(self.file_stream)
        if mode == 'r':
            retval = build_elementary_stream_file(self.fileno,&self.stream)
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
            free_elementary_stream(&self.stream)

    def __iter__(self):
        return self

    def is_readable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'r' and self.stream != NULL

    def is_writable(self):
        """This is a convenience method, whilst reading and writing are exclusive.
        """
        return self.mode == 'w' and self.file_stream != NULL

    # For Pyrex classes, we define a __next__ instead of a next method
    # in order to form our iterator
    def __next__(self):
        """Our iterator interface retrieves the ES units from the stream.
        """
        return _next_ESUnit(self.stream,self.name)

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
        retval = seek_ES(self.stream,where)
        if retval != 0:
            raise TSToolsException,"Error seeking to %s in file '%s'"%(args,self.name)
        else:
            return ESOffset(where.infile,where.inpacket)

    def read(self):
        """Read the next ES unit from this stream.
        """
        try:
            return _next_ESUnit(self.stream,self.name)
        except StopIteration:
            raise EOFError

    def write(self, ESUnit unit):
        """Write an ES unit to this stream.
        """
        if self.file_stream == NULL:
            raise TSToolsException,'ESFile does not seem to have been opened for write'

        retval = write_ES_unit(self.file_stream,unit.unit)
        if retval != 0:
            raise TSToolsException,'Error writing ES unit to file %s'%self.name

    def close(self):
        # Apparently we can't call the __dealloc__ method itself,
        # but I think this is sensible to do here...
        if self.file_stream != NULL:
            retval = fclose(self.file_stream)
            if retval != 0:
                raise TSToolsException,"Error closing file '%s':"\
                        " %s"%(filename,strerror(errno))
        if self.stream != NULL:
            free_elementary_stream(&self.stream)
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

cdef extern from "ts_defns.h":
    struct _ts_reader:
        pass
    ctypedef _ts_reader      TS_reader
    ctypedef _ts_reader     *TS_reader_p

cdef extern from "ts_fns.h":
    int open_file_for_TS_read(char *filename, TS_reader_p *tsreader)
    int close_TS_reader(TS_reader_p *tsreader)
    int seek_using_TS_reader(TS_reader_p tsreader, offset_t posn)
    int read_next_TS_packet(TS_reader_p tsreader, byte **packet)
    int split_TS_packet(byte *buf, PID *pid, int *payload_unit_indicator,
                        byte **adapt, int *adapt_len,
                        byte **payload, int *payload_len)
    int get_next_TS_packet(TS_reader_p tsreader,
                           PID *pid, int *payload_unit_indicator,
                           byte **adapt, int *adapt_len,
                           byte **payload, int *payload_len)


cdef class TSPacket:
    """A convenient representation of a (dissected) TS packet.
    """

    cdef readonly PID       pid
    cdef readonly int       payload_start_indicator
    cdef readonly object    adapt
    cdef readonly object    payload


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
    """

    cdef TS_reader_p    tsreader

    cdef readonly object name
    cdef readonly object mode

    # It appears to be recommended to make __cinit__ expand to take more
    # arguments (if __init__ ever gains them), since both get the same
    # things passed to them. Hmm, normally I'd trust myself, but let's
    # try the recommended route
    def __cinit__(self,filename,mode='r',*args,**kwargs):
        actual_mode = mode+'b'
        if mode == 'r':
            retval = open_file_for_TS_read(filename,&self.tsreader)
            if retval == 1:
                raise TSToolsException,"Error opening file '%s'"\
                        " for TS reading: %s"%(filename,strerror(errno))
        elif mode == 'w':
            raise NotImplemented
        else:
            raise TSToolsException,"Error opening file '%s'"\
                    " with mode '%s' (only 'r' and 'w' supported)"%(filename,mode)

    def __init__(self,filename,mode='r'):
        # What should go in __init__ and what in __cinit__ ???
        self.name = filename
        self.mode = mode

    def __dealloc__(self):
        if self.tsreader != NULL:
            retval = close_TS_reader(&self.tsreader)
            if retval != 0:
                raise TSToolsException,"Error closing file '%s':"\
                        " %s"%(self.name,strerror(errno))

    def __iter__(self):
        return self

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

    # For Pyrex classes, we define a __next__ instead of a next method
    # in order to form our iterator
    def __next__(self):
        """Our iterator interface retrieves the TS packets from the stream.
        """
        #return _next_TS_packet(self.stream,self.name)
        pass

    def seek(self,*args):
        """Seek to the given offset, which should be a multiple of 188.
        """
        pass

    def read(self):
        """Read the next TS packet from this stream.
        """
        try:
            #return _next_TS_packet(self.stream,self.name)
            pass
        except StopIteration:
            raise EOFError

    def write(self, TSPacket tspacket):
        """Write an TS packet to this stream.
        """
        pass

    def close(self):
        pass
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

# ----------------------------------------------------------------------
# vim: set filetype=python expandtab shiftwidth=4:
# [X]Emacs local variables declaration - place us into python mode
# Local Variables:
# mode:python
# py-indent-offset:4
# End:

