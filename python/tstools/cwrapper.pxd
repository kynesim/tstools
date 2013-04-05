"""cwrapper.pxd -- All of the C API for the tstools C library.

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

from common cimport FILE
from common cimport const_void_ptr, const_char_ptr
from common cimport uint8_t, uint16_t, uint32_t, uint64_t
from common cimport int8_t, int16_t, int32_t, int64_t
from common cimport offset_t, byte, PID
from common cimport va_list

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

cdef extern from "ts_defns.h":
    struct _ts_reader:
        pass
    ctypedef _ts_reader      TS_reader
    ctypedef _ts_reader     *TS_reader_p

cdef extern from "pidint_defns.h":
    struct _pidint_list:
        int      *number
        uint32_t *pid
        int       length
        int       size
    ctypedef _pidint_list    pidint_list
    ctypedef _pidint_list   *pidint_list_p
    struct _pmt_stream:
        byte         stream_type
        uint32_t     elementary_PID
        uint16_t     ES_info_length
        byte        *ES_info
    ctypedef _pmt_stream    pmt_stream
    ctypedef _pmt_stream   *pmt_stream_p
    struct _pmt:
        uint16_t     program_number
        byte         version_number
        uint32_t     PCR_pid
        uint16_t     program_info_length
        byte        *program_info
        int          num_streams
        pmt_stream  *streams
    ctypedef _pmt    pmt
    ctypedef _pmt   *pmt_p

cdef extern from "pidint_fns.h":
    void free_pidint_list(pidint_list_p  *list)
    void free_pmt(pmt_p  *pmt)

    void report_pidint_list(pidint_list_p  list,
                            char          *list_name,
                            char          *int_name,
                            int            pid_first)

cdef extern from "ts_fns.h":
    int open_file_for_TS_read(char *filename, TS_reader_p *tsreader)
    int close_TS_reader(TS_reader_p *tsreader)
    int seek_using_TS_reader(TS_reader_p tsreader, offset_t posn)
    int prime_read_buffered_TS_packet(TS_reader_p tsreader, uint32_t pcr_pid)
    int read_next_TS_packet(TS_reader_p tsreader, byte **packet)
    int read_first_TS_packet_from_buffer(TS_reader_p tsreader,
                                         uint32_t pcr_pid, uint32_t start_count,
                                         byte **packet, uint32_t *pid,
                                         uint64_t *pcr, uint32_t *count)
    int read_next_TS_packet_from_buffer(TS_reader_p tsreader,
                                        byte **packet, uint32_t *pid, uint64_t *pcr)
    int split_TS_packet(byte *buf, PID *pid, int *payload_unit_start_indicator,
                        byte **adapt, int *adapt_len,
                        byte **payload, int *payload_len)
    void get_PCR_from_adaptation_field(byte *adapt, int adapt_len, int*got_pcr,
                                       uint64_t *pcr)
    int build_psi_data(int verbose, byte *payload, int payload_len, PID pid,
                       byte **data, int *data_len, int *data_used)
    int find_pat(TS_reader_p tsreader, int max, int verbose, int quiet,
                 int *num_read, pidint_list_p *prog_list)
    int find_next_pmt(TS_reader_p tsreader, uint32_t pmt_pid, int program_number,
                      int max, int verbose, int quiet,
                      int *num_read, pmt_p *pmt)
    int find_pmt(TS_reader_p tsreader, int max, int verbose, int quiet,
                 int *num_read, pmt_p *pmt)
    int extract_prog_list_from_pat(int verbose, byte *data, int data_len,
                                   pidint_list_p *prog_list)
    int extract_pmt(int verbose, byte *data, int data_len, uint32_t pid,
                    pmt_p *pmt)
    int print_descriptors(FILE *stream, char *leader1, char *leader2,
                          byte *desc_data, int desc_data_len)

cdef extern from 'nalunit_defns.h':
    struct nal_unit_context:
        pass
    ctypedef nal_unit_context *nal_unit_context_p
    struct nal_unit:
        pass
    ctypedef nal_unit *nal_unit_p
    struct nal_unit_list:
        nal_unit_p  *array
        int          length
        int          size
    ctypedef nal_unit_list *nal_unit_list_p

cdef extern from 'accessunit_defns.h':
    struct access_unit_context:
        pass
    ctypedef access_unit_context *access_unit_context_p
    struct access_unit:
        uint32_t        index
        # Primary picture?
        int             started_primary_picture
        nal_unit_p      primary_start       # within nal_units
        # Contents
        nal_unit_list_p nal_units

cdef extern from 'nalunit_fns.h':
    int build_nal_unit_context(ES_p es, nal_unit_context_p *context)
    void free_nal_unit_context(nal_unit_context_p *context)
    int rewind_nal_unit_context(nal_unit_context_p context)
    void free_nal_unit(nal_unit_p *nal)
    int find_next_NAL_unit(nal_unit_context_p context, int verbose,
                           nal_unit_p *nal)
    int nal_is_slice(nal_unit_p nal)
    int nal_is_pic_param_set(nal_unit_p nal)
    int nal_is_seq_param_set(nal_unit_p nal)
    int nal_is_redundant(nal_unit_p nal)
    int nal_is_first_VCL_NAL(nal_unit_p nal, nal_unit_p last)

    int build_nal_unit_list(nal_unit_list_p *list)
    int append_to_nal_unit_list(nal_unit_list_p list, nal_unit_p nal)
    void reset_nal_unit_list(nal_unit_list_p list, int deep)
    void free_nal_unit_list(nal_unit_list_p *list, int deep)

cdef extern from 'accessunit_fns.h':
    int build_access_unit_context(ES_p es, access_unit_context_p *context)
    void free_access_unit_context(access_unit_context_p *context)
    int rewind_access_unit_context(access_unit_context_p context)
    void free_access_unit(access_unit_context_p *acc_unit)
    int get_access_unit_bounds(access_unit_context_p access_unit, ES_offset *start,
                               uint32_t *length)
    int all_slices_I(access_unit_context_p access_unit)
    int all_slices_P(access_unit_context_p access_unit)
    int all_slices_I_or_P(access_unit_context_p access_unit)
    int all_slices_B(access_unit_context_p access_unit)
    int get_next_access_unit(access_unit_context_p context, int quiet,
                             int show_details, access_unit_context_p *ret_access_unit)
    int get_next_h264_frame(access_unit_context_p context, int quiet,
                            int show_details, access_unit_context_p *frame)
    int access_unit_has_PTS(access_unit_context_p access_unit)

cdef extern from 'printing_fns.h':
    void print_msg(const_char_ptr text)
    void print_err(const_char_ptr text)
    void fprint_msg(const_char_ptr format, ...)
    void fprint_err(const_char_ptr format, ...)
    int redirect_output( void (*new_print_message_fn) (const_char_ptr message),
                         void (*new_print_error_fn) (const_char_ptr message),
                         void (*new_fprint_message_fn) (const_char_ptr format, va_list arg_ptr),
                         void (*new_fprint_error_fn) (const_char_ptr format, va_list arg_ptr),
                         void (*new_flush_msg_fn) ()
                        )

# This one isn't properly declared - it's not in the header files
cdef extern from *:
    void test_C_printing()


# ----------------------------------------------------------------------
# vim: set filetype=python expandtab shiftwidth=4:
# [X]Emacs local variables declaration - place us into python mode
# Local Variables:
# mode:python
# py-indent-offset:4
# End:
