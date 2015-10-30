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

# GNU conventional destination vars
prefix=/usr/local
exec_prefix=$(prefix)
bindir=$(exec_prefix)/bin
libdir=$(exec_prefix)/lib
mandir=/usr/local/man
man1dir=$(mandir)/man1
manext=.1

INSTALL=install
INSTALL_PROGRAM=$(INSTALL) -m 0555 -s
INSTALL_LIB=$(INSTALL) -m 0444 -s
INSTALL_DATA=$(INSTALL) -m 0444

TSTOOLS_VERSION=1.13
TSTOOLS_LIB_VERSION=1

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
	ARCH_FLAGS =
	# If you're still building on a version of Mac OS X that supports powerpc,
	# then you may want to uncomment the next line. Obviously, this no longer
	# works in Lion, which doesn't support powerpc machines any more.
	#ARCH_FLAGS = -arch ppc -arch i386
else
	SYSTEM = "other"
	ARCH_FLAGS = -fPIC
endif

CFLAGS += $(WARNING_FLAGS) $(OPTIMISE_FLAGS) $(LFS_FLAGS) -I. $(PROFILE_FLAGS) $(ARCH_FLAGS) -DTSTOOLS_VERSION=$(TSTOOLS_VERSION)
LDFLAGS += -g $(PROFILE_FLAGS) $(ARCH_FLAGS) -lm

# Target directories
OBJDIR = obj
LIBDIR = lib
BINDIR = bin
MANDIR = docs/mdoc

# All of our non-program object modules
OBJS = \
 $(OBJDIR)/accessunit.o \
 $(OBJDIR)/avs.o \
 $(OBJDIR)/ac3.o \
 $(OBJDIR)/adts.o \
 $(OBJDIR)/bitdata.o \
 $(OBJDIR)/es.o \
 $(OBJDIR)/filter.o \
 $(OBJDIR)/fmtx.o \
 $(OBJDIR)/h222.o \
 $(OBJDIR)/h262.o \
 $(OBJDIR)/audio.o \
 $(OBJDIR)/l2audio.o \
 $(OBJDIR)/misc.o \
 $(OBJDIR)/nalunit.o \
 $(OBJDIR)/ps.o \
 $(OBJDIR)/pes.o \
 $(OBJDIR)/pidint.o \
 $(OBJDIR)/printing.o \
 $(OBJDIR)/reverse.o \
 $(OBJDIR)/ts.o \
 $(OBJDIR)/tsplay_innards.o \
 $(OBJDIR)/tswrite.o \
 $(OBJDIR)/pcap.o \
 $(OBJDIR)/ethernet.o \
 $(OBJDIR)/ipv4.o

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
  $(OBJDIR)/tsdvbsub.o \
  $(OBJDIR)/tsinfo.o \
  $(OBJDIR)/tsplay.o \
  $(OBJDIR)/tsreport.o \
  $(OBJDIR)/tsserve.o \
  $(OBJDIR)/ts_packet_insert.o \
  $(OBJDIR)/m2ts2ts.o \
  $(OBJDIR)/pcapreport.o  \
  $(OBJDIR)/tsfilter.o

TS2PS_OBJS = $(OBJDIR)/ts2ps.o

TEST_PES_OBJS = $(OBJDIR)/test_pes.o 
TEST_PRINTING_OBJS = $(OBJDIR)/test_printing.o 

TEST_OBJS = \
  $(OBJDIR)/test_nal_unit_list.o \
  $(OBJDIR)/test_es_unit_list.o

# Our library
STATIC_LIB = $(LIBDIR)/libtstools.a
LIBOPTS = $(ARCH_FLAGS) $(STATIC_LIB)

ifeq ($(shell uname -s), Darwin)
SHARED_LIB_NAME = libtstools.xxx
else
SHARED_LIB_NAME = libtstools.so
endif
SHARED_LIB = $(LIBDIR)/$(SHARED_LIB_NAME)

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
  $(BINDIR)/tsdvbsub \
  $(BINDIR)/tsinfo \
  $(BINDIR)/tsreport \
  $(BINDIR)/tsplay \
  $(BINDIR)/tsserve \
  $(BINDIR)/ts_packet_insert \
  $(BINDIR)/m2ts2ts \
  $(BINDIR)/pcapreport \
  $(BINDIR)/tsfilter \
  $(BINDIR)/rtp2264

TS2PS_PROG = $(BINDIR)/ts2ps

# Is test_pes still useful?
TEST_PES_PROG = $(BINDIR)/test_pes 
TEST_PRINTING_PROG = $(BINDIR)/test_printing 

# And then the testing programs (which we only build if we are
# running the tests)
TEST_PROGS = test_nal_unit_list test_es_unit_list

# ------------------------------------------------------------
all:	$(BINDIR) $(LIBDIR) $(OBJDIR) $(PROGS) $(SHARED_LIB)

# ts2ps is not yet an offical program, so for the moment build
# it separately
.PHONY: ts2ps
ts2ps:	$(TS2PS_PROG)

ifeq ($(shell uname -s), Darwin)
# Make libraries containing universal objects on Mac
$(STATIC_LIB): $(OBJS)
	libtool -static $(OBJS) -o $(STATIC_LIB)
$(SHARED_LIB): $(OBJS)
	libtool -dynamic $(OBJS) -o $(SHARED_LIB)
else
$(STATIC_LIB): $(OBJS)
	rm -f $(STATIC_LIB)
	ar rc $(STATIC_LIB) $(OBJS)

$(SHARED_LIB): $(OBJS)
	$(LD) -shared -soname $(SHARED_LIB_NAME).$(TSTOOLS_LIB_VERSION) -o $(SHARED_LIB) $(OBJS) -lc -lm
endif

# Build all of the utilities with the static library, so that they can
# be copied around, shared, etc., without having to think about it

$(BINDIR)/esfilter: $(OBJDIR)/esfilter.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/ts2es: $(OBJDIR)/ts2es.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/es2ts: $(OBJDIR)/es2ts.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/esdots: $(OBJDIR)/esdots.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/esmerge: $(OBJDIR)/esmerge.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/esreport: $(OBJDIR)/esreport.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/esreverse: $(OBJDIR)/esreverse.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/stream_type: $(OBJDIR)/stream_type.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/psreport: $(OBJDIR)/psreport.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/psdots: $(OBJDIR)/psdots.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/ps2ts: $(OBJDIR)/ps2ts.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsinfo: $(OBJDIR)/tsinfo.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsreport: $(OBJDIR)/tsreport.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsserve: $(OBJDIR)/tsserve.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsplay: $(OBJDIR)/tsplay.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/ts_packet_insert: $(OBJDIR)/ts_packet_insert.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/m2ts2ts: $(OBJDIR)/m2ts2ts.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/pcapreport: $(OBJDIR)/pcapreport.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsfilter: $(OBJDIR)/tsfilter.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/tsdvbsub: $(OBJDIR)/tsdvbsub.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/rtp2264: $(OBJDIR)/rtp2264.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)


# Not installed
$(BINDIR)/ts2ps: $(OBJDIR)/ts2ps.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)


$(BINDIR)/test_pes: $(OBJDIR)/test_pes.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/test_printing: $(OBJDIR)/test_printing.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/test_nal_unit_list: $(OBJDIR)/test_nal_unit_list.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

$(BINDIR)/test_es_unit_list: $(OBJDIR)/test_es_unit_list.o $(STATIC_LIB)
	$(CC) $< -o $@ $(LIBOPTS) $(LDFLAGS)

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

# Everyone depends upon the basic configuration file, and I assert they all
# want (or may want) printing...
$(OBJS) $(TEST_OBJS) $(PROG_OBJS): compat.h printing_fns.h

# Which library modules depend on which header files is complex, so
# lets just be simple
$(OBJS): \
                 $(ACCESSUNIT_H) $(NALUNIT_H) $(TS_H) $(ES_H) $(PES_H) \
                 misc_fns.h printing_fns.h $(PS_H) $(H262_H) \
                 $(TSWRITE_H) $(AVS_H) $(REVERSE_H) $(FILTER_H) $(AUDIO_H)

$(OBJDIR)/%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

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
$(OBJDIR)/tsdvbsub.o:     tsdvbsub.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsinfo.o:       tsinfo.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsreport.o:     tsreport.c $(TS_H) fmtx.h misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsserve.o:     tsserve.c $(TS_H) $(PS_H) $(ES_H) misc_fns.h $(PES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/ts_packet_insert.o:     ts_packet_insert.c 
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tsplay.o:       tsplay.c $(TS_H) misc_fns.h $(PS_H) $(PES_H) version.h tsplay_fns.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/tswrite.o:      tswrite.c misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/m2ts2ts.o:	  m2ts2ts.c $(TS_H) misc_fns.h version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/pcapreport.o:      pcapreport.c pcap.h version.h misc_fns.h
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR)/tsfilter.o:      tsfilter.c version.h misc_fns.h
	$(CC) -c $< -o $@ $(CFLAGS)

$(OBJDIR)/test_pes.o: test_pes.c $(TS_H) $(PS_H) $(ES_H) misc_fns.h $(PES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_printing.o: test_printing.c $(TS_H) $(PS_H) $(ES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_nal_unit_list.o: test_nal_unit_list.c $(NALUNIT_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)
$(OBJDIR)/test_es_unit_list.o: test_es_unit_list.c $(ES_H) version.h
	$(CC) -c $< -o $@ $(CFLAGS)

# ------------------------------------------------------------
# Directory creation

$(OBJDIR) $(LIBDIR) $(BINDIR) $(DESTDIR)$(bindir) $(DESTDIR)$(libdir) $(DESTDIR)$(man1dir):
	mkdir -p $@

# ------------------------------------------------------------

.PHONY: install-man
install-man: $(DESTDIR)$(man1dir)
	$(INSTALL_DATA) $(MANDIR)/esfilter.1 $(DESTDIR)$(man1dir)/esfilter$(manext)
	$(INSTALL_DATA) $(MANDIR)/ts2es.1 $(DESTDIR)$(man1dir)/ts2es$(manext)
	$(INSTALL_DATA) $(MANDIR)/es2ts.1 $(DESTDIR)$(man1dir)/es2ts$(manext)
	$(INSTALL_DATA) $(MANDIR)/esdots.1 $(DESTDIR)$(man1dir)/esdots$(manext)
	$(INSTALL_DATA) $(MANDIR)/esmerge.1 $(DESTDIR)$(man1dir)/esmerge$(manext)
	$(INSTALL_DATA) $(MANDIR)/esreport.1 $(DESTDIR)$(man1dir)/esreport$(manext)
	$(INSTALL_DATA) $(MANDIR)/esreverse.1 $(DESTDIR)$(man1dir)/esreverse$(manext)
	$(INSTALL_DATA) $(MANDIR)/stream_type.1 $(DESTDIR)$(man1dir)/stream_type$(manext)
	$(INSTALL_DATA) $(MANDIR)/psreport.1 $(DESTDIR)$(man1dir)/psreport$(manext)
	$(INSTALL_DATA) $(MANDIR)/psdots.1 $(DESTDIR)$(man1dir)/psdots$(manext)
	$(INSTALL_DATA) $(MANDIR)/ps2ts.1 $(DESTDIR)$(man1dir)/ps2ts$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsinfo.1 $(DESTDIR)$(man1dir)/tsinfo$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsreport.1 $(DESTDIR)$(man1dir)/tsreport$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsserve.1 $(DESTDIR)$(man1dir)/tsserve$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsplay.1 $(DESTDIR)$(man1dir)/tsplay$(manext)
	$(INSTALL_DATA) $(MANDIR)/ts_packet_insert.1 $(DESTDIR)$(man1dir)/ts_packet_insert$(manext)
	$(INSTALL_DATA) $(MANDIR)/m2ts2ts.1 $(DESTDIR)$(man1dir)/m2ts2ts$(manext)
	$(INSTALL_DATA) $(MANDIR)/pcapreport.1 $(DESTDIR)$(man1dir)/pcapreport$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsfilter.1 $(DESTDIR)$(man1dir)/tsfilter$(manext)
	$(INSTALL_DATA) $(MANDIR)/tsdvbsub.1 $(DESTDIR)$(man1dir)/tsdvbsub$(manext)
	$(INSTALL_DATA) $(MANDIR)/rtp2264.1 $(DESTDIR)$(man1dir)/rtp2264$(manext)

.PHONY: uninstall-man
uninstall-man:
	rm -f $(DESTDIR)$(man1dir)/esfilter$(manext)
	rm -f $(DESTDIR)$(man1dir)/ts2es$(manext)
	rm -f $(DESTDIR)$(man1dir)/es2ts$(manext)
	rm -f $(DESTDIR)$(man1dir)/esdots$(manext)
	rm -f $(DESTDIR)$(man1dir)/esmerge$(manext)
	rm -f $(DESTDIR)$(man1dir)/esreport$(manext)
	rm -f $(DESTDIR)$(man1dir)/esreverse$(manext)
	rm -f $(DESTDIR)$(man1dir)/stream_type$(manext)
	rm -f $(DESTDIR)$(man1dir)/psreport$(manext)
	rm -f $(DESTDIR)$(man1dir)/psdots$(manext)
	rm -f $(DESTDIR)$(man1dir)/ps2ts$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsinfo$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsreport$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsserve$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsplay$(manext)
	rm -f $(DESTDIR)$(man1dir)/ts_packet_insert$(manext)
	rm -f $(DESTDIR)$(man1dir)/m2ts2ts$(manext)
	rm -f $(DESTDIR)$(man1dir)/pcapreport$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsfilter$(manext)
	rm -f $(DESTDIR)$(man1dir)/tsdvbsub$(manext)
	rm -f $(DESTDIR)$(man1dir)/rtp2264$(manext)

# Shared lib not installed currently
.PHONY: install-prog
install-prog: all $(DESTDIR)$(bindir) $(DESTDIR)$(libdir)
	$(INSTALL_PROGRAM) $(BINDIR)/esfilter $(DESTDIR)$(bindir)/esfilter
	$(INSTALL_PROGRAM) $(BINDIR)/ts2es $(DESTDIR)$(bindir)/ts2es
	$(INSTALL_PROGRAM) $(BINDIR)/es2ts $(DESTDIR)$(bindir)/es2ts
	$(INSTALL_PROGRAM) $(BINDIR)/esdots $(DESTDIR)$(bindir)/esdots
	$(INSTALL_PROGRAM) $(BINDIR)/esmerge $(DESTDIR)$(bindir)/esmerge
	$(INSTALL_PROGRAM) $(BINDIR)/esreport $(DESTDIR)$(bindir)/esreport
	$(INSTALL_PROGRAM) $(BINDIR)/esreverse $(DESTDIR)$(bindir)/esreverse
	$(INSTALL_PROGRAM) $(BINDIR)/stream_type $(DESTDIR)$(bindir)/stream_type
	$(INSTALL_PROGRAM) $(BINDIR)/psreport $(DESTDIR)$(bindir)/psreport
	$(INSTALL_PROGRAM) $(BINDIR)/psdots $(DESTDIR)$(bindir)/psdots
	$(INSTALL_PROGRAM) $(BINDIR)/ps2ts $(DESTDIR)$(bindir)/ps2ts
	$(INSTALL_PROGRAM) $(BINDIR)/tsinfo $(DESTDIR)$(bindir)/tsinfo
	$(INSTALL_PROGRAM) $(BINDIR)/tsreport $(DESTDIR)$(bindir)/tsreport
	$(INSTALL_PROGRAM) $(BINDIR)/tsserve $(DESTDIR)$(bindir)/tsserve
	$(INSTALL_PROGRAM) $(BINDIR)/tsplay $(DESTDIR)$(bindir)/tsplay
	$(INSTALL_PROGRAM) $(BINDIR)/ts_packet_insert $(DESTDIR)$(bindir)/ts_packet_insert
	$(INSTALL_PROGRAM) $(BINDIR)/m2ts2ts $(DESTDIR)$(bindir)/m2ts2ts
	$(INSTALL_PROGRAM) $(BINDIR)/pcapreport $(DESTDIR)$(bindir)/pcapreport
	$(INSTALL_PROGRAM) $(BINDIR)/tsfilter $(DESTDIR)$(bindir)/tsfilter
	$(INSTALL_PROGRAM) $(BINDIR)/tsdvbsub $(DESTDIR)$(bindir)/tsdvbsub
	$(INSTALL_PROGRAM) $(BINDIR)/rtp2264 $(DESTDIR)$(bindir)/rtp2264

.PHONY: uninstall-prog
uninstall-prog:
	rm -f $(DESTDIR)$(bindir)/esfilter
	rm -f $(DESTDIR)$(bindir)/ts2es
	rm -f $(DESTDIR)$(bindir)/es2ts
	rm -f $(DESTDIR)$(bindir)/esdots
	rm -f $(DESTDIR)$(bindir)/esmerge
	rm -f $(DESTDIR)$(bindir)/esreport
	rm -f $(DESTDIR)$(bindir)/esreverse
	rm -f $(DESTDIR)$(bindir)/stream_type
	rm -f $(DESTDIR)$(bindir)/psreport
	rm -f $(DESTDIR)$(bindir)/psdots
	rm -f $(DESTDIR)$(bindir)/ps2ts
	rm -f $(DESTDIR)$(bindir)/tsinfo
	rm -f $(DESTDIR)$(bindir)/tsreport
	rm -f $(DESTDIR)$(bindir)/tsserve
	rm -f $(DESTDIR)$(bindir)/tsplay
	rm -f $(DESTDIR)$(bindir)/ts_packet_insert
	rm -f $(DESTDIR)$(bindir)/m2ts2ts
	rm -f $(DESTDIR)$(bindir)/pcapreport
	rm -f $(DESTDIR)$(bindir)/tsfilter
	rm -f $(DESTDIR)$(bindir)/tsdvbsub
	rm -f $(DESTDIR)$(bindir)/rtp2264

.PHONY: install
install: install-man install-prog

.PHONY: uninstall
uninstall: uninstall-man uninstall-prog

.PHONY: objclean
objclean:
	-rm -f $(OBJS)
	-rm -f $(TEST_OBJS)
	-rm -f $(TEST_PROGS)
	-rm -f $(TS2PS_OBJS) $(TS2PS_PROG)
	-rm -f $(TEST_PES_OBJS) $(TEST_PES_PROG)
	-rm -f $(TEST_PRINTING_OBJS) $(TEST_PRINTING_PROG)
	-rm -f ES_test3.ts  es_test3.ts
	-rm -f ES_test2.264 es_test3.264
	-rm -f es_test_a.ts es_test_a.264
	-rm -f es_test_b.ts es_test_b.264
	-rm -f *.core

.PHONY: clean
clean: objclean
	-rm -f $(PROGS)
	-rm -f $(STATIC_LIB)
	-rm -f $(SHARED_LIB)
	-rm -f $(PROG_OBJS)

.PHONY: distclean
distclean: clean
	-rm -rf $(OBJDIR) $(LIBDIR) $(BINDIR)
	rm -f debian/files debian/tstools.*
	rm -rf debian/tstools

.PHONY: dist
dist: distclean
	ln -snf `pwd` ../tstools-$(TSTOOLS_VERSION)
	tar czhf ../tstools-$(TSTOOLS_VERSION).tar.gz ../tstools-$(TSTOOLS_VERSION)

.PHONY: dist-debian
dist-debian: dist
	ln -snf tstools-$(TSTOOLS_VERSION).tar.gz ../tstools_$(TSTOOLS_VERSION).orig.tar.gz
	debuild -uc -us

TESTDATAFILE = /data/video/CVBt_hp_trail.264

# Only build test_printing if explicitly asked to do so
.PHONY: test_printing
test_printing: $(BINDIR)/test_printing

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
