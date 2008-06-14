================
TS tools library
================

.. contents::


Relevant International Standards
================================

- ISO/IEC 13818-1 (H.222.0) *Information technology - Generic coding of moving
  pictures and associated audio information: Systems*

  This describes:

  - TS (Transport Stream)
  - PS (Program Stream)
  - ES (Elementary Stream) and
  - PES (Packetised Elementary Stream)

  which form the transport layers for the following standards.

- ISO/IEC 13818-2 (H.262) *Information technology - Generic coding of moving
  pictures and associated audio information: Video*

   This defines MPEG-2.

- ISO/IEC 14496-10 (H.264)

   This defines MPEG-4/AVC.

Overview of modules
===================

Standalone header files:

:compat.h:
  Defines useful types for portability between Unices and Windows (for
  instance, the basic integer types and ``offset_t``, which is a 64 or
  32 bit file offset as appropriate).

:h222_defns.h:
  Defines various values useful when using H.222.


Source files:

:accessunit.c:
  Handling H.264 access units, including reading them in as NAL units.

:adts.c:
  Some minimal support for ISO/IEC 14496-3:2001(E) AAC ADTS audio streams -
  basically what is needed by the `esmerge` tool.

:bitdata.c:
  Handling bit level data, including reading Exp-Golomb encoded values. Used
  by the NAL unit reading functions in nalunit.c.

:es.c:
  Reading and writing at an Elementary Stream level.

:filter.c:
  Fast forward algorithms.

:h262.c:
  Handling H.262 pictures.

:misc.c:
  As it says, various things that provide miscellaneous support.

:nalunit.c:
  Handling H.264 NAL units, mainly as a base for access units.

:pes.c:  
  Reading PS or TS data, and extracting PES therefrom. Used as a level
  under es.c to allow reading of ES data from PS and TS files.

:pidint.c:
  Handling "dictionaries" of PIDs versus integers (for instance, PID and
  program stream).

:ps.c:
  Provides the ``ps_to_ts`` function, which forms the basis of the ps2ts tool.

:reverse.c:
  Reversing algorithms and support.

:ts.c:
  Reading and writing Transport Stream.

:tswrite.c:
  Support for writing Transport Stream packets, either to a file, over TCP/IP
  or (via a circular buffer) over UDP. This thus provides support for all
  tools that have a ``-host`` switch, and also the bulk of the functionality
  of tsplay. Also provides the code that allows tsserve to read command
  characters from a socket.

Each source file *xxx* also has associated with it a header file defining
datastructures, constants and macros, called *xxx*\ _defns.h, and a header file
detailing ``extern`` functions therefrom, called *xxx*\ _fns.h. The latter will
always include the former.

The documentation for each ``extern`` function is reproduced in the header
file, directly copied from the source. This is done for the convenience of the
user, but if any discrepancy occurs, the version of the functionc header
comment in the source file should be taken as correct.

Not all ``extern`` functions are intended for use by end-users. Some are
really only used within the library itself. Unfortunately, these functions are
not flagged as such at the moment.

Reading data
============
In general, the various MPEG entities are not read directly from a file, but
through a context datastructure.

For instance, reading an access unit may stop before a particular NAL unit,
which thus forms the start of the next access unit, and NAL units themselves
need to be interpreted in the context of sequence and picture parameter sets.

These are arranged roughly as follows::

                +-------------------+  (r)  +---------------+
                | H.264 access unit |       | H.262 context |
                |       context     |       +---------------+
                +-------------------+	          :
		           :			  :
			   :			  :
                 +------------------+ (*)	  :
                 | NAL unit context |		  :
                 +------------------+		  :
			   :			  :
			   :			  :
                         +---------------------------+
                         |         ES context        |
                         +---------------------------+
			   :			  :
			   :			  :
                  +------------------+		  :
                  |    PES reader    |		  :
                  +------------------+		  :
		       :	  :		  :
                +-----------+   +-----------+	  :
                | TS reader |   | PS reader |	  :
                +-----------+   +-----------+	  :
		           :      :		  :
			   :	  :		  :
                         +---------------------------+
                         |           File            |
                         +---------------------------+

:(r): Both H.264 and H.262 contexts can be associated with a "reversing"
      context, to accumulate data for outputting the stream in (fast) reverse.

:(*): A NAL unit context is created implicitly when building an access unit
      context "over" an ES context.


Access units, H.264, MPEG-4/AVC
-------------------------------
An access unit context is explicitly built on top of an ES context::

   err = build_access_unit_context(es,&acontext);
   free_access_unit_context(&acontext);

Freeing the access unit context does not free the ES context.

As well as maintaining the information to allow reading access units, the
context also remembers any trailing (end of sequence or end of stream) NAL
units. This is mostly transparent to the user, but is explained in the
appropriate function header comments.

An individual access unit can be retrieved::

   err = get_next_access_unit(acontext,quiet,show_details,&access_unit);

but it is more normal to retrieve a frame::

   err = get_next_h264_frame(acontext,quiet,show_details,&frame);

If the frame was composed of two access units (i.e., two fields), then the NAL
units for the second will have been appended to the first, which is returned,
and its field/frame indicator will have been set to "frame".

Regardless, the same function is used to free the resultant datastructure::

   free_access_unit(&frame);

Access units may be written to ES or TS::

   err = write_access_unit_as_ES(access_unit,context,filedesc);
   err = write_access_unit_as_TS(access_unit,context,tswriter,video_pid);

Note that the latter assumes that the video stream id is 0xE0. Variants are
alsp provided to output PTS and/or PCR values for the first PES packet written
out.

A report on the content of an access unit can be obtained with::

   report_access_unit(filedesc,access_unit);

Various utility functions are provided to investigate the properties of a
particular access unit::

   all_I = all_slices_I(access_unit);
   all_P = all_slices_P(access_unit);
   all_IP = all_slices_I_or_P(access_unit);
   all_B = all_slices_B(access_unit);

Lastly, an access unit context can be rewound with::

   err = rewind_access_unit_context(acontext);

H.262 pictures, MPEG-2 and MPEG-1
---------------------------------
For most purposes, MPEG-1 data is supported as a subset of MPEG-2.

An H.262 context is explicitly built on top of an ES context::

   err = build_h262_context(es,&context);
   free_h262_context(&context);

Freeing the H.262 context does not free the ES context.

An individual H.262 picture can be retrieved::

   err = get_next_h262_single_picture(context,verbose,&picture);

but it is more normal to retrieve a frame::

   err = get_next_h262_frame(context,verbose,quiet,&frame);

If the frame was composed of two field pictures, then the H.262 items
for the second will have been appended to the first, which is returned,
and its field/frame indicator will have been set to "frame".

Regardless, the same function is used to free the resultant datastructure::

   free_h262_picture(&frame);

Pictures may be written to ES or TS::

   err = write_h262_picture_as_ES(filedesc,picture);
   err = write_h262_picture_as_TS(tswriter,picture,video_pid);

Note that the latter assumes that the video stream id is 0xE0.

A report on the content of a picture can be obtained with::

   report_h262_picture(filedesc,picture,report_data);

Lastly, an H.262 context can be rewound with::

   err = rewind_h262_context(context);


Below the picture level
-----------------------
H.264 access units are composed from NAL units, read with an underlying NAL
unit context (which is created automatically within an access unit context).
The NAL unit context is then retrievable as ``acontext->nac``.

A NAL unit context may also be created (and then freed) directly::

   err = build_nal_unit_context(es,&context);
   free_nal_unit_context(context);

The NAL unit context remembers the picture and sequence parameter sets for the
H.264 data stream.

From whatever source, the NAL unit context can be used to read NAL units
directly (although doing this with the ``nac`` from an access unit context
will disrupt access unit reading)::

   err = find_next_NAL_unit(context,verbose,&nal);
   free_nal_unit(&nal);

Functions also exist to report on an individual NAL unit, and to write it out
as ES or TS data.

H.262 pictures are composed of individual units as well, although there does
not appear to be a standard name for these. The H.262 context manages their
reading directly, and they may also be read individually (although doing so
will disrupt H.262 picture reading)::

   err = find_next_h262_item(es,&item);

Again, functions are provided to report on such an item, or write it out as ES
or TS.

Each NAL unit or MPEG-2 item contains a single ES unit (which is why the
contexts used to read them and their higher level data constructs require an
ES context).

Elementary Stream data
----------------------
Various ways are provided to open an Elementary Stream. The simplest opens a
file containing "bare" ES data::

   err = open_elementary_stream(filename,&es);

If a PES reader is available (for reading TS or PS data), then an elementary
stream can be constructed atop that::

   err = build_elementary_stream_PES(pes_reader,&es);

Once the elementary stream is available, however, its underlying form does not
matter, and it can normally be closed with::

   close_elementary_stream(&es);

(this will not "close" a PES reader if one is involved).

Functions are then provided to read in individual ES units, although in
practice the higher level (H.264 access unit and H.262 picture) functions will
be used to read data.


PES reading - TS and PS data
----------------------------
PES data may be encapsulated as either PS or TS. The normal way to open a PES
reader is with::

   err = open_PES_reader(filename,give_info,give_warnings,&reader);

which will inspect the start of the file to work out if it is PS or
TS. Alternatively, if it is known which the file is, then one can directly
call::

   err = open_PES_reader_for_PS(filename,give_info,give_warnings,&reader);
   err = open_PES_reader_for_TS(filename,program_number,
                                give_info,give_warnings,&reader);

(the latter must also be used if one wants a different program number than the
"first found" in TS data). The function::

   err = determine_if_TS_file(filedesc,&is_TS);

may also be used to figure out if an already opened file is TS, and that
may then be wrapped in a reader::

   err = build_PES_reader(filedesc,is_TS,give_info,give_warnings,
                          program_number,&reader);

If a PS or TS reader context is already built, then they may be wrapped within
a PES reader::

   err = build_TS_PES_reader(tsreader,give_info,give_warnings,program_number,
                             &reader);
   err = build_PS_PES_reader(psreader,give_info,give_warnings,&reader);

When finished with, the PES reader may be freed or closed (the latter also
closes the PS/TS reader and underlying file)::

   err = free_PES_reader(&reader);
   err = close_PES_reader(&reader);

It is possible to request that only video be read from the reader::

   set_PES_reader_video_only(reader);

or that audio be taken from Private Stream 1 (normally used for Dolby), as
opposed to the "normal" audio streams::

   set_PES_reader_audio_private1(reader);

For PS data, which does not have PAT/PMT packets to describe the program being
read, it is possible to set various key pieces of information::

   set_PES_reader_program_data(reader,program_number,pmt_pid,
                               video_pid,audio_pid,pcr_pid);

In situations where the software has "guessed" wrongly whether the data is
H.262 or H.264, or where data is being read from standard input and it did not
have an opportunity to decide, it is possible to insist::

   set_PES_reader_h264(reader);

(the default is H.262).

PES packets may be read individually, but this is normally mediated by one of
the higher levels.

Server mode
...........
It is possible to associate a Transport Stream writer with the PES input
stream. This is then used to "mirror" each PES packet, so that the input
stream is automatically written out as TS (specifically, each time a new PES
packet is read in, the previous packet is written out).

Where to write the data is specified with::

   set_server_output(reader,tswriter,program_freq);

This also starts the mirroring. ``program_freq`` is how often (in PES packets)
the PAT/PMT program information should be written out.

Mirroring may be switched on and off using::

   start_server_output(reader);
   stop_server_output(reader);

``tsserve`` is the main program that takes advantage of this capability -
using it whilst moving linearly forwards in the data is simple enough, but if
one needs to fast forwards or move backwards, things rapidly become more
complex.


Read-ahead buffers
==================
Since the bottom-most file access is done via file descriptors, there is no
system-provided buffering.

Currently, read-ahead buffers are provided by:

* The TS reader
* The "bare" ES reader (i.e., reading bytes directly from a file)

In both of these contexts, ``ftell`` cannot usefully be used to determine
where in the file the application is/will be reading - instead, the TS reader
context and ES context maintain their own notions of current position, which
should be used instead.

Rewinding
=========
As a rule, when rewinding a data stream, use the rewind function for
the "highest level" context available.

Thus if reading access units, use ``rewind_access_unit_context``, rather than
(for instance) ``seek_ES``.

General seeking within files above the ES level has not been implemented, as
none of the existing tools require it.

Reversing
=========
For issues when reversing H.262 data, see the documentation for ``esreverse``
in the Tools_ document.

.. _Tools: tools.html

Reversing of H.264 currently uses non-IDR frames more than it should. This is
primarily because the Harry Potter clip only has a single IDR, and thus it has
been difficult to be sure what to do. Unfortunately, in H.264, B and P frames
can refer back before the last I frame, so just outputting a couple of
reference frames does not guarantee a coherent picture when the next
non-reference frame is encountered. The solution is to enfore output of IDR
frames at such transitions, and this will be investigated later on.


Reversing in the library is handed in a relatively "black box" manner. A
reverse data context must be built::

   err = build_reverse_data(&reverse_data,is_h264);

and then added to the appropriate H.262 picture or H.264 access unit context::

   err = add_h262_reverse_context(context,reverse_data);
   err = add_access_unit_reverse_context(acontext,reverse_data);

(this could obviously use some streamlining). After this, normal reading of
frames in the forwards direction remembers appropriate reversing information.

Alternatively, the reversing data for a whole file can be accumulated with one
call (it just processes through the file)::

   err = collect_reverse_h262(context,max,verbose,quiet);
   err = collect_reverse_access_units(acontext,max,verbose,quiet,
                                      seq_param_data,pic_param_data);

Data may be output in reverse using the appropriate call - these are the same
for H.262 and H.264 data::

   err = output_in_reverse_as_ES(es,filedesc,freqency,verbose,quiet,
                                 start_with,max,reverse_data);
   err = output_in_reverse_as_TS(es,tswriter,verbose,quiet,offset,
                                 start_with,max,reverse_data);

``start_with`` indicates which frame to start reversing from - ``-1`` means
the "current" picture. ``frequency`` indicates the speed of reversing required
- thus a value of ``8`` means reversing at (about) 8 times.

The reversing datastructures can be freed when no longer needed::

   free_reverse_data(reverse_data);

but this does not detach them from the H.262 or H.264 context, so should only
be used when tidying up those datastructures.

Filtering
=========
For issues when filtering data, see the documentation for ``esfilter``
in the Tools_ document.

.. _Tools: tools.html

An appropriate filter context is built::

   err = build_h262_filter_context_strip(&fcontext,context,all_IP);
   err = build_h262_filter_context(&fcontext,context,frequency);
   err = build_h264_filter_context_strip(&fcontext,acontext,all_ref);
   err = build_h264_filter_context(&fcontext,acontext,frequency);

and later freed::

   free_h262_filter_context(fcontext);
   free_h264_filter_context(fcontext);

For the stripping contexts, the ``all_IP`` flag means keep all I *and* P
frames (rather than just I), and the ``all_ref`` flag means keep all reference
pictures.

For the filtering contexts, the ``frequency`` is the speedup that is required
- for instance, a value of ``8`` means that 8x fast forward is desired.

These may then be used to retrieve the next appropriate frame from the input
stream::

   err = get_next_stripped_h262_frame(fcontext,verbose,quiet,
                                      &seq_hdr,&frame,&frames_seen);
   err = get_next_filtered_h262_frame(fcontext,verbose,quiet,
                                      &seq_hdr,&frame,&frames_seen);

   err = get_next_stripped_h264_frame(fcontext,verbose,quiet,
                                      &frame,&frames_seen);
   err = get_next_filtered_h264_frame(fcontext,verbose,quiet,
                                      &frame,&frames_seen);

In all cases, the caller must free ``frame`` when they have finished with
it. However, for H.262 data, ``seq_hdr`` must not be freed.

When filtering, ``frame`` is returned as NULL to indicate that the previous
frame should be repeated, to produce (an approximation to) the desired
frequency.

.. ***** BEGIN LICENSE BLOCK *****

License
=======
Version: MPL 1.1

The contents of this file are subject to the Mozilla Public License Version
1.1 (the "License"); you may not use this file except in compliance with
the License. You may obtain a copy of the License at
http://www.mozilla.org/MPL/

Software distributed under the License is distributed on an "AS IS" basis,
WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
for the specific language governing rights and limitations under the
License.

The Original Code is the MPEG TS, PS and ES tools.

The Initial Developer of the Original Code is Amino Communications Ltd.
Portions created by the Initial Developer are Copyright |copy| 2008
the Initial Developer. All Rights Reserved.

.. |copy| unicode:: 0xA9 .. copyright sign

Contributor(s):

  Amino Communications Ltd, Swavesey, Cambridge UK

.. ***** END LICENSE BLOCK *****
.. -------------------------------------------------------------------------------
.. vim: set filetype=rst expandtab shiftwidth=2:
