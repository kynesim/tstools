"""common.pyd -- Common definitions

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

cdef extern from "stdlib.h":
    cdef void free(void *ptr)

# From the Cython FAQ, but according to a useful message on the Pyrex mailing
# list, also applicable to Pyrex
cdef extern from *:
    ctypedef void* const_void_ptr "const void*"
    ctypedef char* const_char_ptr "const char*"

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

    # Returns a pointer to a read-only memory location containing arbitrary
    # data. The obj argument must support the single-segment readable buffer
    # interface. On success, returns 0, sets buffer to the memory location and
    # buffer_len to the buffer length. Returns -1 and sets a TypeError on
    # error.
    int PyObject_AsReadBuffer(object obj, const_void_ptr *buffer, Py_ssize_t *buffer_len) except -1

    # Unfortunately, there are two common ways of implementing a va_list,
    # and we just have to guess which is being used. For the moment, though,
    # just take advantage of the fact that the following seems to work for
    # our purposes...
    ctypedef void * va_list

cdef extern from "Python.h":
    FILE *PySys_GetFile(char *name, FILE *default)

cdef extern from "stdint.h":
    ctypedef unsigned char      uint8_t
    ctypedef unsigned           uint16_t
    ctypedef unsigned long      uint32_t
    ctypedef unsigned long long uint64_t
    ctypedef   signed char      int8_t
    ctypedef          int       int16_t
    ctypedef          long      int32_t
    ctypedef          long long int64_t

# PIDs are too long for 16 bits, short enough to fit in 32
ctypedef uint32_t   PID

cdef extern from "compat.h":
    # We don't need to define 'offset_t' exactly, just to let Pyrex
    # know it's vaguely int-like
    ctypedef int offset_t
    ctypedef uint8_t byte  # but we already had our stdint byte daatype

# ----------------------------------------------------------------------
# vim: set filetype=python expandtab shiftwidth=4:
# [X]Emacs local variables declaration - place us into python mode
# Local Variables:
# mode:python
# py-indent-offset:4
# End:
