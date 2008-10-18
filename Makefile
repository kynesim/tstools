# Makefile for the H.264 Elementary Stream software
# - temporarily hacked to work on Mac OS/X 10.5 (Leopard)
#
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
#   Amino Communications Ltd, Swavesey, Cambridge UK
#
# ***** END LICENSE BLOCK *****
#
### RUN WITH GNU MAKE

# Gnu make recommends always setting some standard variables
SHELL = /bin/sh

# And re-establishing the required suffix list
.SUFFIXES:
.SUFFIXES: .c .o

ifdef CROSS_COMPILE
CC = $(CROSS_COMPILE)gcc
else
CC = gcc
endif

# Use WARN=1 periodically to get too many warnings...
ifdef WARN
WARNING_FLAGS = -Wall -W -Wfloat-equal -Wundef -Wshadow -Wpointer-arith -Wcast-qual -Wconversion -Wmissing-prototypes -Wmissing-declarations -Wunreachable-code -Winline
else
WARNING_FLAGS = -Wall
endif

# Use NOOPT=1 if using valgrind --tool=memcheck/addrecheck
ifdef NOOPT
OPTIMISE_FLAGS = -g
else
OPTIMISE_FLAGS = -O2 -g
endif

# Use PROFILE=1 to allow use of gprof (but this is *not* needed for valgrind)
ifdef PROFILE
PROFILE_FLAGS = -pg
else
PROFILE_FLAGS = 
endif

# On Linux, large file support is not necessarily enabled. To make programs
# assume large file support, it is necessary to build them with _FILE_OFFSET_BITS=64.
# This replaces the "standard" short file operations with equivalent large file
# operations.
# On (Free)BSD, this is not necessary, but conversely it does not look like defining
# the flags will have any effect either.
LFS_FLAGS = -D_FILE_OFFSET_BITS=64

# Try for a best guess whether this is a Mac running OS/X, or some other
# sort of thing (presumably Linux or BSD)
ifeq ($(shell uname -s), Darwin)
	SYSTEM = "macosx"
	ARCH_FLAGS = -arch ppc -arch i386
else
	SYSTEM = "other"
	ARCH_FLAGS =
endif

CFLAGS = $(WARNING_FLAGS) $(OPTIMISE_FLAGS) $(LFS_FLAGS) -I. $(PROFILE_FLAGS) $(ARCH_FLAGS)
LDFLAGS = -g -lm $(PROFILE_FLAGS) $(ARCH_FLAGS)

# Target directories
OBJDIR = obj
LIBDIR = lib
BINDIR = bin

# All of our non-program source files
SRCS = \
 accessunit.c \
 ac3.c \
 adts.c \
 avs.c \
 bitdata.c \
 es.c \
 fmtx.c \
 h222.c \
 h262.c \
 audio.c \
 l2audio.c \
 misc.c \
 nalunit.c \
 ps.c \
 pes.c \
 pidint.c \
 ts.c \
 tswrite.c \
 pcap.c 

# All of our non-program object modules
OBJS = \
 accessunit.o \
 avs.o \
 ac3.o \
 adts.o \
 bitdata.o \
 es.o \
 filter.o \
 fmtx.o \
 h222.o \
 h262.o \
 audio.o \
 l2audio.o \
 misc.o \
 nalunit.o \
 ps.o \
 pes.o \
 pidint.o \
 reverse.o \
 ts.o \
 tswrite.o \
 pcap.o \
 ethernet.o \
 ipv4.o

# Our program object modules
PROG_OBJS = \
  $(OBJDIR)/es2ts.o \
  $(OBJDIR)/esdots.o \
  $(OBJDIR)/esfilter.o \
  $(OBJDIR)/esmerge.o \
  $(OBJDIR)/esreport.o \
  $(OBJDIR)/esreverse.o \
  $(OBJDIR)/ps2ts.o \
  $(OBJDIR)/psreport.o \
  $(OBJDIR)/psdots.o \
  $(OBJDIR)/stream_type.o \
  $(OBJDIR)/ts2es.o \
  $(OBJDIR)/tsinfo.o \
  $(OBJDIR)/tsplay.o \
  $(OBJDIR)/tsreport.o \
  $(OBJDIR)/tsserve.o \
  $(OBJDIR)/ts_packet_insert.o \
  $(OBJDIR)/m2ts2ts.o \
  $(OBJDIR)/pcapreport.o 
#\
#  $(OBJDIR)/test_ps.o

TS2PS_OBJS = $(OBJDIR)/ts2ps.o

TEST_PES_OBJS = $(OBJDIR)/test_pes.o 

TEST_OBJS = \
  $(OBJDIR)/test_nal_unit_list.o \
  $(OBJDIR)/test_es_unit_list.o

# Our library
LIB = $(LIBDIR)/libtstools.a
LIBOPTS = -L$(LIBDIR) -ltstools $(ARCH_FLAGS)

# All of our programs (except the testing ones)
PROGS = \
  $(BINDIR)/esfilter \
  $(BINDIR)/ts2es \
  $(BINDIR)/es2ts \
  $(BINDIR)/esdots \
  $(BINDIR)/esmerge \
  $(BINDIR)/esreport \
  $(BINDIR)/esreverse \
  $(BINDIR)/ps2ts \
  $(BINDIR)/psreport \
  $(BINDIR)/psdots \
  $(BINDIR)/stream_type \
  $(BINDIR)/tsinfo \
  $(BINDIR)/tsreport \
  $(BINDIR)/tsplay \
  $(BINDIR)/tsserve \
  $(BINDIR)/ts_packet_insert \
  $(BINDIR)/m2ts2ts \
  $(BINDIR)/pcapreport 
#\
#  $(BINDIR)/test_ps

TS2PS_PROG = $(BINDIR)/ts2ps

# Is test_pes still useful?
TEST_PES_PROG = $(BINDIR)/test_pes 

# And then the testing programs (which we only build if we are
# running the tests)
TEST_PROGS = test_nal_unit_list test_es_unit_list

# ------------------------------------------------------------
all:	$(BINDIR) $(LIBDIR) $(OBJDIR) $(PROGS)

# ts2ps is not yet an offical program, so for the moment build
# it separately
.PHONY: ts2ps
ts2ps:	$(TS2PS_PROG)

ifeq ($(shell uname -s), Darwin)
# Try getting a library containing universal objects on Mac
$(LIB): $(OBJS)
	libtool -static $(OBJS) -o $(LIB)
else
$(LIB): $(LIB)($(OBJS))
endif

$(BINDIR)/esfilter:	$(OBJDIR)/esfilter.o $(LIB)
		$(CC) $< -o $(BINDIR)/esfilter $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/ts2es:		$(OBJDIR)/ts2es.o $(LIB)
		$(CC) $< -o $(BINDIR)/ts2es $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/es2ts:		$(OBJDIR)/es2ts.o $(LIB)
		$(CC) $< -o $(BINDIR)/es2ts $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/esdots:		$(OBJDIR)/esdots.o $(LIB)
		$(CC) $< -o $(BINDIR)/esdots $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/esmerge:	$(OBJDIR)/esmerge.o $(LIB)
		$(CC) $< -o $(BINDIR)/esmerge $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/esreport:	$(OBJDIR)/esreport.o $(LIB)
		$(CC) $< -o $(BINDIR)/esreport $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/esreverse:	$(OBJDIR)/esreverse.o $(LIB)
		$(CC) $< -o $(BINDIR)/esreverse $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/stream_type:	$(OBJDIR)/stream_type.o $(LIB)
		$(CC) $< -o $(BINDIR)/stream_type $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/psreport:	$(OBJDIR)/psreport.o $(LIB)
		$(CC) $< -o $(BINDIR)/psreport $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/psdots:	$(OBJDIR)/psdots.o $(LIB)
		$(CC) $< -o $(BINDIR)/psdots $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/ps2ts:		$(OBJDIR)/ps2ts.o $(LIB)
		$(CC) $< -o $(BINDIR)/ps2ts $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/tsinfo:		$(OBJDIR)/tsinfo.o $(LIB)
		$(CC) $< -o $(BINDIR)/tsinfo $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/tsreport:	$(OBJDIR)/tsreport.o $(LIB)
		$(CC) $< -o $(BINDIR)/tsreport $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/tsserve:	$(OBJDIR)/tsserve.o $(LIB)
		$(CC) $< -o $(BINDIR)/tsserve $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/tsplay:		$(OBJDIR)/tsplay.o $(LIB)
		$(CC) $< -o $(BINDIR)/tsplay $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/test_ps:	$(OBJDIR)/test_ps.o $(LIB)
		$(CC) $< -o $(BINDIR)/test_ps $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/ts2ps:		$(OBJDIR)/ts2ps.o $(LIB)
		$(CC) $< -o $(BINDIR)/ts2ps $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/ts_packet_insert:	$(OBJDIR)/ts_packet_insert.o $(LIB)
		$(CC) $< -o $(BINDIR)/ts_packet_insert $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/m2ts2ts:		$(OBJDIR)/m2ts2ts.o $(LIB)
		$(CC) $< -o $(BINDIR)/m2ts2ts $(LDFLAGS) $(LIBOPTS)
$(BINDIR)/pcapreport:	$(OBJDIR)/pcapreport.o $(LIB)
		$(CC) $< -o $(BINDIR)/pcapreport $(LDFLAGS) $(LIBOPTS)




$(BINDIR)/test_pes:	$(OBJDIR)/test_pes.o $(LIB)
		$(CC) $< -o $(BINDIR)/test_pes $(LDFLAGS) $(LIBOPTS)

$(BINDIR)/test_nal_unit_list: 	$(OBJDIR)/test_nal_unit_list.o $(LIB)
			$(CC) $< -o $(BINDIR)/test_nal_unit_list $(LDFLAGS) $(LIBOPTS)
$(BINDIR)/test_es_unit_list:  	$(OBJDIR)/test_es_unit_list.o $(LIB)
			$(CC) $< -o $(BINDIR)/test_es_unit_list $(LDFLAGS) $(LIBOPTS)

# Some header files depend upon others, so including one requires
# the others as well
ES_H = es_fns.h es_defns.h h222_fns.h h222_defns.h
TS_H = ts_fns.h ts_defns.h h222_fns.h h222_defns.h tswrite_fns.h \
       tswrite_defns.h pidint_fns.h pidint_defns.h
ACCESSUNIT_H = accessunit_fns.h accessunit_defns.h $(NALUNIT_H)
NALUNIT_H = nalunit_fns.h nalunit_defns.h es_fns.h es_defns.h \
            bitdata_fns.h bitdata_defns.h
PES_H = pes_fns.h pes_defns.h
PS_H = ps_fns.h ps_defns.h
AVS_H = avs_fns.h avs_defns.h
H262_H = h262_fns.h h262_defns.h
TSWRITE_H = tswrite_fns.h tswrite_defns.h
REVERSE_H = reverse_fns.h reverse_defns.h
FILTER_H = filter_fns.h filter_defns.h $(REVERSE_H)
AUDIO_H = adts_fns.h l2audio_fns.h ac3_fns.h audio_fns.h audio_defns.h adts_defns.h

# Everyone depends upon the basic configuration file
$(LIB)($(OBJS)) $(TEST_OBJS) $(PROG_OBJS): compat.h

# Which library modules depend on which header files is complex, so
# lets just be simple
$(LIB)($(OBJS)): $(ACCESSUNIT_H) $(NALUNIT_H) $(TS_H) $(ES_H) $(PES_H) \
                 misc_fns.h $(PS_H) $(H262_H) $(TSWRITE_H) $(AVS_H) \
                 $(REVERSE_H) $(FILTER_H) $(AUDIO_H)

$(OBJDIR)/es2ts.o:        es2ts.c $(ES_H) $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/esdots.o:       esdots.c misc_fns.h $(ACCESSUNIT_H) $(H262_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/esfilter.o:     esfilter.c $(TS_H) misc_fns.h $(ACCESSUNIT_H) $(H262_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/esreport.o:     esreport.c misc_fns.h $(ACCESSUNIT_H) $(H262_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/esmerge.o:     esmerge.c misc_fns.h $(ACCESSUNIT_H) $(AUDIO_H) $(TSWRITE_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/esreverse.o:    esreverse.c $(TS_H) $(REVERSE_H) misc_fns.h $(ACCESSUNIT_H) $(H262_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/fmtx.o:         fmtx.c fmtx.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/psreport.o:     psreport.c $(ES_H) $(PS_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/psdots.o:     psdots.c $(ES_H) $(PS_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/ps2ts.o:        ps2ts.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/stream_type.o:  stream_type.c $(ES_H) $(TS_H) $(NALUNIT_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/ts2es.o:        ts2es.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/ts2ps.o:        ts2ps.c $(TS_H) $(PS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsinfo.o:       tsinfo.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsreport.o:     tsreport.c $(TS_H) fmtx.h misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsserve.o:     tsserve.c $(TS_H) $(PS_H) $(ES_H) misc_fns.h $(PES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/ts_packet_insert.o:     ts_packet_insert.c 
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsplay.o:       tsplay.c $(TS_H) misc_fns.h $(PS_H) $(PES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tswrite.o:      tswrite.c misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/m2ts2ts.o:	  m2ts2ts.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/pcapreport.o:      pcapreport.c pcap.h version.h misc_fns.h
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR)/test_pes.o: test_pes.c $(TS_H) $(PS_H) $(ES_H) misc_fns.h $(PES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_ps.o: test_ps.c $(PS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_nal_unit_list.o: test_nal_unit_list.c $(NALUNIT_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_es_unit_list.o: test_es_unit_list.c $(ES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)

# ------------------------------------------------------------
# Directory creation

$(OBJDIR):
	mkdir $(OBJDIR)

$(LIBDIR):
	mkdir $(LIBDIR)

$(BINDIR):
	mkdir $(BINDIR)

# ------------------------------------------------------------
.PHONY: objclean
objclean:
	-rm -f $(OBJS)
	-rm -f $(TEST_OBJS)
	-rm -f $(TEST_PROGS)
	-rm -f $(TS2PS_OBJS) $(TS2PS_PROG)
	-rm -f $(TEST_PES_OBJS) $(TEST_PES_PROG)
	-rm -f ES_test3.ts  es_test3.ts
	-rm -f ES_test2.264 es_test3.264
	-rm -f es_test_a.ts es_test_a.264
	-rm -f es_test_b.ts es_test_b.264
	-rm -f *.core

.PHONY: clean
clean: objclean
	-rm -f $(PROGS)
	-rm -f $(LIB)
	-rm -f $(PROG_OBJS)

.PHONY: distclean
distclean: clean
	-rmdir $(OBJDIR)
	-rmdir $(LIBDIR)
	-rmdir $(BINDIR)

TESTDATAFILE = /data/video/CVBt_hp_trail.264

# Only build test_pes if explicitly asked to do so
.PHONY: test_pes
test_pes: $(BINDIR)/test_pes

.PHONY: test
test:   test_lists

.PHONY: test_lists
test_lists:	$(BINDIR)/test_nal_unit_list  $(BINDIR)/test_es_unit_list
	@echo +++ Testing NAL unit lists
	$(BINDIR)//test_nal_unit_list
	@echo +++ Test succeeded
	@echo +++ Testing ES unit lists
	$(BINDIR)/test_es_unit_list
	@echo +++ Test succeeded
