"""tstools.pyx -- Pyrex bindings for the TS tools
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

cdef extern from 'es_defns.h':
    # The reader for an ES file
    struct elementary_stream:
        pass
    ctypedef elementary_stream  ES
    ctypedef elementary_stream *ES_p

    # An actual ES unit
    struct ES_unit:
        pass
    ctypedef ES_unit *ES_unit_p

cdef extern from 'es_fns.h':
    int open_elementary_stream(char *filename, ES_p *es)
    void close_elementary_stream(ES_p *es)
    int find_and_build_next_ES_unit(ES_p es, ES_unit_p *unit)
    void free_ES_unit(ES_unit_p *unit)
    void report_ES_unit(FILE *stream, ES_unit_p unit)

# Is this the best thing to do?
class TSToolsException(Exception):
    pass
    
cdef class ESStream:
    """A Python class representing an ES stream, readable from a file.
    """

    cdef ES_p stream
    cdef readonly object filename

    # It appears to be recommended to make __cinit__ expand to take more
    # arguments (if __init__ ever gains them), since both get the same
    # things passed to them. Hmm, normally I'd trust myself, but let's
    # try the recommended route
    def __cinit__(self,filename,*args,**kwargs):
        retval = open_elementary_stream(filename,&self.stream)
        print 'retval',retval
        print 'stream %d'%<int>self.stream
        if retval != 0:
            raise TSToolsException,'Error opening ES file %s'%filename

    def __init__(self,filename):
        # The __cinit__ method will already have *used* the filename,
        # but it may be useful to remember it for later on
        self.filename = filename

    def __dealloc__(self):
        close_elementary_stream(&self.stream)
