========
TS Tools
========

.. contents::

Overview
========
The following tools are provided:

:es2ts_:       Read ES (video), output TS
:esdots_:      Print one character per ES unit
:esfilter_:    "Fast forward" ES video data to a file (outputs ES or TS)
:esmerge_:     Merge H.264 video and AAC ADTS audio ES to TS (very specific)
:esreport_:    Report on the contents of an ES file
:esreverse_:   "Reverse" ES video data to a file (outputs ES or TS)
:ps2es:        Use ts2es_ (``ts2es -pes``) to obtain the effect of this.
:ps2ts_:       Read PS data, output TS
:psdots_:      Print one character per PS packet
:psreport_:    Report on the contents of a PS file
:stream_type_: Make a (barely) educated guess what a file contains
:ts2es_:       Extract an ES stream from a TS file
:tsinfo_:      Report program info for a TS file (summarise PAT/PMT info)
:tsplay_:      Play (and possibly loop) a PS/TS file over UDP (using timing
               info) or TCP
:tsreport_:    Report on the contents of a TS file
:tsserve_:     Serve PS/TS files to clients (multicast) over TCP

There are also some test programs, which are not otherwise discussed:

:test_es_unit_list:   Test the working of ES unit lists
:test_nal_unit_list:  Test the working of NAL unit lists (built on the above)
:test_pes:            Test the working of PES reading

It is the intention that all the tools should work on Linux, Windows, Mac OS/X
and BSD, although not all variants will always be tested at all times.


Common syntax and assumptions
=============================

In all of the tools, the switches and filenames (when used) may be mixed at
will - specifically, switches are allowed after filenames. This makes it more
convenient to run a particular program more than once by repeating the
previous command but appending new switches.

All switches are indicated by a single introductory ``-``. No provision is
made for coping with filenames that start with ``-``.

All tools have the following command line options in common:

:``-h``, ``-help``, ``--help``:
                    Provide help text about the program. There may also
                    be more detailed help texts for some programs, available
                    via different commands, which this help text will explain.

                    Note that all of the tools will provide this help text
                    if they are run with no arguments.

:``-q``, ``-quiet``:
                    Output no text except error messages.

:``-v``, ``-verbose``:
                    Output extra text. What this is depends on the type of
                    application; it might be extra information (for a report
                    tool), or debugging information (for a processing tool).

Note that verbose and quiet are [#]_ mutually incompatible. That is, there are
only three states:

  1. "normal" (not verbose, not quiet)
  2. verbose (and not quiet)
  3. quiet (and not verbose)

.. [#] or should be - some programs might not yet enforce this.

Several tools share the following switches:

:``-tsout``:       Output TS instead of ES (common amongst ES processing
                   tools).

:``-host <name>``, |hostandport|:
                   Specify a host to output to, and optionally a port
                   number. The default port number is 88.

.. We can't put a colon in a field lists :field name:, so use an indirection...
.. |hostandport| replace:: ``-host <name>:<port>``

:``-output <name>``, ``-o <name>``:
                   Specify output to the named file.

:``-pid <pid>``:   Specify a PID. This is read as decimal by default, but
                   a hexadecimal value can be specifed as (for instance)
                   ``-pid 0x68``. The specific switch ``-pid`` is not common,
                   but variants are.

:``-stdin``, ``-stdout``:
                   Take input from the standard input, write output to the
                   standard output. If -stdout is used, -quiet is always
                   enforced. If input is from standard input, the tool will
                   not be able to "guess" the input type -- tools will
                   generally default to H.262 (MPEG-2) in this case.

:``-h262``:        Indicates that the input (video) data is H.262 data
                   (actually, either MPEG-1 or MPEG-2). Stops the tool trying
                   to determine this for itself.

:``-h264``, ``-avc``:
                   Indicates that the input (video) data is H.264 (i.e.,
                   MPEG-4/AVC). Stops the tools trying to determine this for
                   itself.

:``-dvd``, ``-notdvd``, ``-nodvd``:
                   Indicates that the PS data being read conforms (or doesn't)
                   to DVD conventions. This matters for audio data in
                   private_stream_1, which is packaged as "substreams" in DVD
                   data. The programs ps2ts and psreport assume DVD data
                   (since program stream is normally obtained from DVDs).

:``-dolby dvb``, ``-dolby atsc``:
                   Dolby (AC-3) audio data may be written out using either of
                   two TS stream types, 0x06 for DVB data, and 0x81 for ATSC
                   data. When reading TS data, the stream type read is used
                   for output, but when reading PS data and outputting TS
                   data, a decision must be made. The default is to use the
                   DVB convention. This switch is provided for ps2ts, tsplay
                   and tsserve.

:``-max <n>``:     Some of the tools can stop after reading/processing <n>
                   of the appropriate data items. This can (for instance)
                   be used to truncate data files, or to inspect only part
                   of a data stream.

:``-pes``:         Some of the ES reading tools will instead read from TS or
                   PS data if given the ``-pes`` switch. This saves piping
                   data through ts2es or (for PS) ps2ts and ts2es.

For all of the tools, the documentation provided by ``-help`` should be used
to find current command line definitions - these are not necessarily repeated
below.


Error and warning messages
==========================
Conventionally, error and warning messages are all output to ``stderr``.

Error messages are prefixed by ``###``, and warning messages by ``!!!``.

Errors may be expected to cause a tool to exit with "failure" status.
The final (outermost) message in a sequence of error messages for a particular
tool will indicate the tool name (this can be useful when piping several tools
together).


Common considerations
=====================

Support for MPEG-1
------------------
MPEG-1 is essentially a subset of MPEG-2. The tools provided do not explicitly
support MPEG-1, but should be lax enough in their requirements for MPEG-2 to
allow MPEG-1 data as well (for instance, not *requiring* the presence of
Sequence Extensions). In general, if this document talks about MPEG-2 (or
H.262), MPEG-1 may be assumed as well.

Determining the file type
-------------------------
Several of the tools attempt to determine what type of data is contained in
the input file. The mechanism used opts for simplicity over rigour, and can
thus conceivably make a mistake. In this case, the user should use the
appropriate switch (e.g., ``-h262`` or ``-h264``) to override it.

H.264 profiles
--------------
The underlying H.264 (MPEG-4/AVC) library checks the flags in the first
sequence parameter set to determine what H.264 profile the data claims to be
following. This library supports the "main" profile (profile indicator 77),
or other profiles if they declare that they only contain data from the "main"
profile.

If the first sequence parameter set indicates that the bistream contains a
different protocol, and does not indicate that it is conforming to the "main"
profile, then the library will output a warning message - for instance::

    Warning: This bitstream declares itself as extended profile (88).
             This software does not support extended profile,
             and may give incorrect results or fail.

It will continue regardless, however, as experience shows that this
information is not always presented correctly.

    (Note that although this is a warning message it is not prefixed
    by ``!!!``. It is, however, still output to standard error.)

Frames and fields
-----------------
If the input data is being treated in terms of frames, the tools will all
aggregate individual fields into "frames" (by appending the NAL units or ES
units of the second field to the first).

In MPEG-2 processing tools, sequence headers are (broadly) treated as pictures
(and thus as frames), and are thus included in the frame count.

In MPEG-4/AVC data, the way that access units are aggregated can (when an end
of stream unit is found) lead to an apparent "extra" access unit at the end of
the data.

Broken Program Stream files
---------------------------
`Determining the file type`_ above explained how the tools are tolerant of
PS files that do not start with a pack header.

When reading PS data, if the start of a packet is being read (i.e., bytes 0x00
0x00 0x01 0x*XX*, where *XX* is the packet's stream type) and some other
sequence of bytes is found, then the software will scan forwards for the next
pack header. If no next pack header is found, then it will exit with an error.

This should cope with files that have "dropped" data, where some bytes have
been lost. It is unlikely to help directly with files that have corrupted
data, which will cause errors at higher levels (for instance, in the H.262
"picture" building routines).


es2ts
=====
Converts an elementary video stream to transport stream.

For instance::

    $ es2ts  hp-trail.264  hp-trail.ts
    $ es2ts  hp-trail.264  -host norton

or, taking the newly created TS file from above, extracting its video stream
using ts2es_ and outputting it anew with video in PID 0x99::

    $ ts2es  -video -stdout hp-trail.ts | es2ts -stdin -pid 0x99 hp-trail99.ts

The input stream may be MPEG-1 (as a subset of MPEG-2), MPEG-2 or
MPEG-4/AVC. By default the tool looks at the start of the input file to
determine whether it is MPEG-2 or MPEG-4/AVC.

Input may be a file or standard input.

Output may be a file, standard output or a host via TCP.

The maximum number of ES units to process may be specified with the ``-max``
switch.

No decoding of the contents of ES units is made, so the only reason that the
tool needs to know whether the data is MPEG-2 or MPEG-4/AVC is so that it can
write appropriate TS.



esdots
======
This is a diagnostic tool used to gain a "view" of the contents of a data
file.

For instance::

    $ esdots -v hp-trail.264
    Reading from hp-trail.264
    Input appears to be MPEG-4/AVC (H.264)

    Warning: This bitstream declares itself as extended profile (88).
             This software does not support extended profile,
             and may give incorrect results or fail.

    Ipppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp
    pppppppppppppppppppppppppppppippppppppppppppppppppppppppppppppppppppppp
    ... lines omitted ...
    pppppppppppppppippppppppppppppppppppppppppppppppppppppppppppppp
    Found 2550 NAL units in 2548 access units
    0 IDR, 5 I, 78 P, 0 B access units
    GOP size (s): max=1.1200, min=0.0400, mean=0.67682

    $ esdots /data/CharliesAngels.es -max 1000
    Reading from /data/CharliesAngels.es
    Input appears to be MPEG-2 (H.262)
    [E>iEUUbEUbEUpEUbEUbEUpEUbEUbEUpEUbEUbEU[E>iEUUbEUbEUpEUbEUbEUpEUbEUbEU
    ... lines omitted ...
    pEUbEUbEU[E>iEUUbEU
    Found 1000 MPEG2 items
    30 I, 60 P, 98 B
    GOP times (s): max=0.4800, min=0.1200, mean=0.448125


If the ``-v`` switch is used, then an explanation of the meaning of the
characters output will be prepended (it is slightly different depending on
whether the input is MPEG-2 or MPEG-4/AVC).

If the ``-gop`` switch is used, then each GOP duration is displayed. For
H.264, where the GOP is not defined, we retrieve this data by measuring the
time between two random access points.
It looks like this::

    [E>iEbEbEpEbEbEpEbEbEpEbEbE: 0.4800s
    [E>iEbEbE: 0.1200s
    [E>iEbEbEpEbEbEpEbEbEpEbEbE: 0.4800s
    ...

The gop times are computed supposing thatthe frame rate is 25 fps. If this is not the case, the value can be changed using the -fr switch (e.g. "-fr 30").

esfilter
========
Reads an input video stream and outputs it to another file, possibly filtering
the data.

For instance::

    $ esfilter  -copy  some-file.es  shorter-copy.es  -max 2000

just copies the first 2,000 ES units from one file to the other, ::

    $ esfilter  -strip  some-file.es  stripped-file.es

outputs just the I (and for H.264, IDR) frames, whilst::

    $ esfilter  -filter  some-file.es  filtered-file.es  -max 2000

attempts to "fast forward" the input file, outputting the result, but stopping
after 2,000 frames. The default "speed-up" is 8x, which may be altered with
the ``-freq`` switch.

Input is from an elementary stream, either an explicit file or standard input.

Output can be to elementary stream (the default), or to transport
stream (with the ``-tsout`` switch). For either, standard output can be chosen
instead of an explicit file. If TS output is specified, writing to a host
over TCP/IP can also be requested.

Note that for ``-strip`` and ``-filter``, the ``-max`` switch acts on the number
of frames, but since ``-copy`` works at the ES item/NAL unit level, its ``-max``
works on these lower level entities.

Fast forward algorithms
-----------------------
The simpler "strip" algorithm, acts by simply discarding all frames that are
not I (or, for MPEG-4/AVC, IDR) frames. This is quick, and produces a result
guaranteed to display without artefacts (since I and IDR frames do not refer
outside themselves), but will produce a variable speed-up depending on the
distribution of the different sorts of frame within the data.

If the ``-allref`` switch is supplied as well, then the "strip" algorithm is
modified to keep all reference frames in MPEG-4/AVC, and all I and P frames
in MPEG-2. This will produce a lesser speed-up, which should still display
safely.

The more complex "filter" algorithm attempts to emulate a specific speed-up,
defaulting to 8x (and alterable with the ``-freq`` switch). It may repeat
frames to produce the requested speed.

In either case, the output frames are not altered in any way, which means that
the resultant data stream is unlikely to be technically valid. For instance,
in the MPEG-4/AVC case, no attempt is made to amend frame numbers. Also,
metadata may not be correct, since (in MPEG-4/AVC) only the first sequence and
picture parameter sets found are output.


esmerge
=======
This utility was written specifically to merge an MPEG-4/AVC video stream with
an AAC ADTS audio stream.

For instance::

    $ esmerge video.264 audio.aac result.ts

It is also possible to specify the audio rate (which defaults to 44.1KHz).

Timing information is output (based on 25 video frames/second and the given
audio rate, assuming 1024 samples per frame) as follows:

1. In the PES PTS and TS PCR for the first NAL unit of every I or IDR video
   frame. The PCR is generated by using the PTS as its base and 0 as its
   extension.
2. In the TS PCR for the first NAL unit of every other video frame.
3. In the PES PTS and TS PCR for the first NAL unit of every audio frame.

Whilst very specific at the moment, this tool could obviously be expanded to
be more versatile if future needs require.

   Since original writing, some support for AVS (video) and MPEG layer 2
   (audio) has been added.


esreport
========
Reads an input elementary stream (or, with ``-pes``, PS or TS) and reports on
it.

For instance, for MPEG-2::

    $ esreport  -max 5 CharliesAngels.es
    Reading from CharliesAngels.es
    Input appears to be MPEG-2 (H.262)
    00000000/0000: MPEG2 item b3 (SEQUENCE HEADER) size 140
    00000140/0000: MPEG2 item b5 (Extension start) size 10
    00000150/0000: MPEG2 item b8 (Group start) size 8
    00000158/0000: MPEG2 item 00 (Picture) 1 (I) size 8
    00000166/0000: MPEG2 item b5 (Extension start) size 9
    Found 5 MPEG-2 items

Where the format is:

    start_pos_in_file/start_pos_in_packet: MPEG2 item unit_start_code (explanation of unit_start_code and additional info if it is a picture) data_length

Or, for AVC::

    $ esreport -max 5 hp-trail.264
    Reading from hp-trail.264
    Input appears to be MPEG-4/AVC (H.264)
    00000001/0000: NAL unit 3/7 (seq param set)          11: 67 58 00 15 96 53 01 68 24 88...

    Warning: This bitstream declares itself as extended profile (88).
             This software does not support extended profile,
             and may give incorrect results or fail.

    00000015/0000: NAL unit 3/8 (pic param set)           5: 68 ce 38 80 00
    00000023/0000: NAL unit 3/5 (IDR)                  3191: 65 88 80 40 02 13 14 00 04 2f...
    00003217/0000: NAL unit 2/1 (non-IDR)              5453: 41 9a 02 05 84 01 c5 d4 7d 88...
    00008673/0000: NAL unit 2/1 (non-IDR)              7558: 41 9a 04 09 41 00 71 1a f8 ff...

    Stopping because 5 NAL units have been read
    Found 5 NAL units
    nal_ref_idc:
             2 of  2
             3 of  3
    nal_unit_type:
             2 of  1 (non-IDR)
             1 of  5 (IDR)
             1 of  7 (seq param set)
             1 of  8 (pic param set)
    slice_type:
             2 of  5 (All P)
             1 of  7 (All I)

Where the format is:

    start_pos_in_file/start_pos_in_packet: NAL unit nal_ref_idc/nal_unit_type (explanation of nal_unit_type) data_length(in bytes):first_data_bytes

Or, at frame level::

    $ esreport  -frames -max 5 CharliesAngels.es
    Reading from CharliesAngels.es
    Input appears to be MPEG-2 (H.262)
    Sequence header: frames and fields
    I Frame          #2
    B Frame          #0
    B Frame          #1
    P Frame          #5
    Found 5 MPEG-2 pictures

The number after the ``#`` is the frames temporal reference, and the "picture"
count includes the sequence header.

If ``-q`` is specified, only the final counts are output.

The ``-x`` switch shows details of each NAL unit as it is read.


esreverse
=========
Reads an input video stream and outputs it to another file, in "fast reverse".

For instance::

    $ esreverse  hp-trail.264  hp-reverse.264

Output may optionally be to Transport Stream, instead of ES::

    $ esreverse  hp-trail.264  -tsout  hp-reverse.ts

TS or PS data may be read (instead of ES) using the ``-pes`` switch::

    $ esreverse  -pes  CVBt_hp_trail.ts  -tsout  CVBt_reversed.ts

Note that this will only read (and reverse) the video stream. 

The "frequency" of frames to try to keep when reversing the data (thus the
"speedup" in reversing) may be specified with the ``-freq`` switch. It
defaults to 8.

Reverse algorithms
------------------
The input data is scanned forwards. For MPEG-2, the location and index of I
frames and sequence headers is remembered. For MPEG-4/AVC, the location and
index of I and IDR frames is remembered.

Reversing is then done by starting at the end of the array of remembered data,
and moving backwards, attempting to reproduce the requsted frequency. This may
entail repeating particular frames.

In MPEG-2 data, each I frame "remembers" the sequence header that precedes
it, and also the AFD that is applicable to itself. When outputting the reverse
data, a section header is output if appropriate (i.e., if it not the same as
for the "previous" I frame), but AFDs are output for each frame.

In MPEG-4/AVC data, sequence parameter set and picture parameter set NAL units
must be output at the start of the output. ``esreverse`` writes out the data
for the last sequence and picture parameters sets found with each id
(typically, this means sequence parameter set 0 and picture parameter set 0)
at the start of the reversed output.


ps2ts
=====
Reads an input Program Stream and outputs equivalent Transport Stream.

For instance::

    $ ps2ts  CharliesAngels.mpg  CharliesAngels.ts

One video stream is supported. If multiple audio streams are found in the PS,
then the first will be output (unless ``-noaudio`` is used to suppress it).

The video, audio and PMT PIDs may be specified.

The video data will be output to TS with stream type 0x02 for MPEG-2 video,
and 0x1b for MPEG-4/AVC video.

An attempt is made to work out an appropriate stream type for the audio,
depending upon what it is. Note that AC3 on DVD is stored in substreams,
which must be "unpacked" when outputting the data as TS.

PS program stream map and program stream directory are ignored, mainly because
I have not yet seen data with these present.

When writing the video data, the SCR base and extension from the PS pack
header are used as the PCR base and extension.

.. Note:: Reading PS data may be slower than reading ES or TS data.


psdots
======
This is a diagnostic tool used to gain a "view" of the contents of a data
file.

For instance::

    $ psdots -v CharliesAngels.mpg -max 100
    Reading from CharliesAngels.mpg
    Stopping after 100 PS packets
    [Hv[v[v[v[a[a[v[v[v[v[v[v[a[v[v[v[v[v[v[v[v[v[v[a[a[v[v[v[v[v[v[v[v[v[v
    [v[v[a[a[v[v[v[v[v[v[v[v[v[v[v[a[v[v[v[v[v[v[v[v[v[v[v[v[a[a[v[v[v[v[v[
    v[v[v[v[v[v[v[v[v[v[v[a[a[v[v[v[v[v[v[v[v[v[v[v[v[a[v[v[v[v
    Stopping after 100 packs

If the ``-v`` switch is used, then an explanation of the meaning of the
characters output will be prepended.


psreport
========
Reports on the content of a Program Stream.

For instance::

    $ psreport CharliesAngels.mpg
    Reading from CharliesAngels.mpg
    Packets (total):            984111
    Packs:                      492055
    Video packets (stream  0):  426945  min size  3748, max size  7178, mean size  7042.7
    Audio packets (stream  0):   65110  min size   602, max size  5888, mean size  3556.2
    Program stream maps:             0
    Program stream directories:      0

Information about the packets can be obtained with the ``-v`` switch::

    $ psreport CharliesAngels.mpg -v -max 3
    Reading from CharliesAngels.mpg
    Stopping after 3 PS packets

    00000000: Pack header: SCR 0 (0/0) mux rate 18020
    00000014: System header 1
                Read 1 system header
    00000032: PS Packet  3 stream E0 (Video stream 0x0)
                   Packet (7168 bytes): 00 00 01 e0 1b fa 8f c0 0a 21 00 07 6d c5 11 00 07 19 65 00...

    00007200: Pack header: SCR 214800 (716/0) mux rate 18020
                Read 0 system headers
    00007214: PS Packet  5 stream E0 (Video stream 0x0)
                   Packet (7168 bytes): 00 00 01 e0 1b fa 8b 00 01 ff eb 12 6b 21 0a f5 86 04 a4 eb...

    a00014382: Pack header: SCR 429600 (1432/0) mux rate 18020
                      Read 0 system headers
    00014396: PS Packet  7 stream E0 (Video stream 0x0)
                   Packet (7168 bytes): 00 00 01 e0 1b fa 8b 00 01 ff 25 7f b2 10 8d 75 bb 00 6b c9...
    Stopping after 3 packs
    Packets (total):                 8
    Packs:                           3
    Video packets (stream  0):       3  min size  7168, max size  7168, mean size  7168.0
    Program stream maps:             0
    Program stream directories:      0


stream_type
===========
Looks at the start of a file to attempt to determine if it is:

      * Transport Stream
      * Program Stream
      * Elementary Stream containing MPEG-2
      * Elementary Stream containing MPEG-4/AVC
      * PES

The mechanisms used to decide are not very sophisticated, but appear to work
in practice.

For instance::

    $ stream_type  CharliesAngels.mpg
    Reading from CharliesAngels.mpg
    It appears to be Program Stream

or, with "explanations" enabled::

    $ stream_type  CharliesAngels.mpg  -v
    Reading from CharliesAngels.mpg
    Trying to read pack header
    File starts 00 00 01 BA - could be PS, reading pack header body
    OK, trying to read start of next packet
    Start of second packet found at right place - looks like PS
    It appears to be Program Stream

``stream_type`` returns an exit value which may be used in shell scripts to
take action according to its decision.


ts2es
=====
Extract a single Elementary Stream from Transport Stream.

For instance::

    $ ts2es  -video  CharliesAngels.ts  CharliesAngels.es

The ES to be extracted may be the video stream (taken to be the first video
stream found), the first audio stream found, or the stream with a specific
PID.

The ``-pes`` switch may be used to read data via the PES reading mechanisms,
which allows ``ts2es`` to read PS data as well::

    $ ts2es -pes  CharliesAngels.mpg  CharliesAngels.es


tsinfo
======
Present information on the program streams within a TS file.

For instance::

    $ tsinfo  CVBt_hp_trail.ts
    Reading from CVBt_hp_trail.ts
    Scanning 1000 TS packets

    Packet 9 is PAT
    Program list:
        Program 1 -> PID 0066 (102)

    Packet 10 is PMT with PID 0066 (102)
    Program streams:
        PID 0068 (104) -> Stream type  27 (H.264/14496-10 video (MPEG-4/AVC))
        PID 0067 (103) -> Stream type  15 (13818-7 Audio with ADTS transport syntax)
    PCR PID 0068 (104)

    Found 1 PAT packet and 1 PMT packet in 1000 TS packets

The tool looks through the first 1000 TS packets for PAT and PMT entries, and
reports on their content. If multiple PAT/PMT entries are found, with
differing content, then this will be reported::

    $ tsinfo  CharliesAngels.ts
    Reading from CharliesAngels.ts
    Scanning 1000 TS packets

    Packet 9 is PAT
    Program list:
        Program 1 -> PID 0066 (102)

    Packet 10 is PMT with PID 0066 (102)
    Program streams:
        PID 0068 (104) -> Stream type   2 (H.262/13818-2 video (MPEG-2) or 11172-2 constrained video)
    PCR PID 0068 (104)

    Packet 168 is PMT with PID 0066 (102) - content changed
    Program streams:
        PID 0068 (104) -> Stream type   2 (H.262/13818-2 video (MPEG-2) or 11172-2 constrained video)
    PID 0067 (103) -> Stream type   4 (13818-3 audio (MPEG-2))
    PCR PID 0068 (104)

    Found 2 PAT packets and 2 PMT packets in 1000 TS packets


tsplay
======
Plays TS data over UDP or TCP/IP.

Multicasting is supported over UDP::

    $ tsplay  hp-trail.ts  255.1.1.1

Playing can be looped (i.e., to repeat indefinitely)::

    $ tsplay  hp-trail.ts  255.1.1.1  -loop

TCP/IP may also be used::

    tsplay  hp-trail.ts  -tcp  norton 

When playing over UDP, the tool manages a circular buffer of TS packets, which
are output to the target host at appropriate times, based on the TS packet
PCR.

Best response will be obtained with a fast machine and locally stored data.

Note that if output is over UDP, and output is to a multicast IP address,
then the network interface to use for the multicast broadcast can be chosen
with the ``-mcastif`` switch. For example::

    tsplay hp-trail.ts 235.1.1.1:1234 -mcastif 192.168.172.12

This option may not be supported on some versions of Windows.

There are many tuning options - see ``-help tuning`` for details. The most
useful are probably:

* ``-maxnowait`` -- This specifies how many packets (each of 7 TS packets) may
  be sent to the client without a gap. The default is 3, which is relatively
  conservative. If the output is choppy, and tsplay reports that it is having
  to restart its timing::

     !!! Packet 701 (item 101): Outputting 0.5s late - restarting time sequence
         Maybe consider running with -maxnowait greater than 3

  then increasing the ``-maxnowait`` value may help (a value of 10 is
  reasonable if the client process is happy with this).

* ``-hd`` -- This is provided for playing HD video, and is a "short hand"
  instead of specifying a variety of other switches (including ``-maxnowait``)
  with suitable values.

Circular buffer algorithm
-------------------------
This is only used for output over UDP - it is not applicable to TCP/IP.

Transport Stream packets are read in batches of 7 (chosen to give a sensible
number of bytes for sending over the network - seven TS packets is 1316 bytes,
which will fit within an ethernet packet of size 1500). These "batches" are
written to a circular buffer. Each "batch" has a timestamp, derived from the
PCRs in the Transport Stream.

A child process is forked, which reads the "batch" entries from the circular
buffer, and attempts to output over UDP at an appropriate time. If it believes
that it is sending too fast, it will wait. If it believes that it is sending
too slow, it will output multiple "batches" with no intervening gap, up to a
maximum number (as mentioned above).

A slow machine will find that it is always trying to catch up, and will not
"feed" the UDP client adequately.

Almost all aspects of the algorithm can be changed. Details are available via
the ``-tuning`` switch. 


tsreport
========
Reports on the TS packets in a file.

For instance::

    $ tsreport CharliesAngels.ts
    Reading from CharliesAngels.ts
    !!! 156 bytes ignored at end of file - not enough to make a TS packet
    Read 382583 TS packets

More information can be obtained with the ``-v`` switch::

    $ tsreport CharliesAngels.ts -v -max 20
    Reading from CharliesAngels.ts
    Stopping after 20 TS packets
    00000000: TS Packet  1 PID 1fff PADDING - ignored
    00000188: TS Packet  2 PID 1fff PADDING - ignored
    00000376: TS Packet  3 PID 1fff PADDING - ignored
    00000564: TS Packet  4 PID 1fff PADDING - ignored
    00000752: TS Packet  5 PID 1fff PADDING - ignored
    00000940: TS Packet  6 PID 1fff PADDING - ignored
    00001128: TS Packet  7 PID 1fff PADDING - ignored
    00001316: TS Packet  8 PID 1fff PADDING - ignored
    00001504: TS Packet  9 PID 0000 [pusi] PAT
      section length:        00d (13)
      transport stream id: 0001
      version number 00, current next 1, section number 0, last section number 0
        Program 001 (  1) -> PID 0066 (102)
    00001692: TS Packet 10 PID 0066 [pusi] PMT
      section length:   012 (18)
      program number: 0001
      version number 00, current next 1, section number 0, last section number 0
      PCR PID: 0068
      program info length: 0
        Stream 02 (H.262/13818-2 video (MPEG-2) or 11172-  -> PID 0068 Info (0 bytes)
    00001880: TS Packet 11 PID 0068 [pusi]
      Adaptation field len   7 [flags 10]: PCR
     .. PCR            0
      PES header
        Start code:        00 00 01
        Stream ID:         e0   (224)
        PES packet length: 1bfa (7162)
        Flags:             8f c0 PES-priority  data-aligned  copyright  original/copy : PTS  DTS
        PES header len 10
    !!! Guard bits at start of PTS data are 2, not 3
        PTS 112354
        DTS 101554
    00002068: TS Packet 12 PID 0068
    00002256: TS Packet 13 PID 0068
    00002444: TS Packet 14 PID 0068
    00002632: TS Packet 15 PID 0068
    00002820: TS Packet 16 PID 0068
    00003008: TS Packet 17 PID 0068
    00003196: TS Packet 18 PID 0068
    00003384: TS Packet 19 PID 0068
    00003572: TS Packet 20 PID 0068
    Stopping after 20 packets
    Read 20 TS packets

The ``-timing`` switch may be used to obtain PCR timing information::

    $ tsreport CharliesAngels.ts -timing -max 500
    Reading from CharliesAngels.ts
    Stopping after 500 TS packets
     .. PCR            0
     .. PCR       214800 Mean byterate  921620 byterate  921620
     .. PCR       429600 Mean byterate  921620 byterate  921620
     .. PCR       644400 Mean byterate  921620 byterate  921620
     .. PCR      5298300 Mean byterate  182986 byterate   80711
     .. PCR      5513100 Mean byterate  211764 byterate  921620
     .. PCR      5727900 Mean byterate  238384 byterate  921620
     .. PCR      5942700 Mean byterate  263080 byterate  921620
     .. PCR      6157500 Mean byterate  286053 byterate  921620
     .. PCR      6372300 Mean byterate  307477 byterate  921620
     .. PCR     10430700 Mean byterate  222394 byterate   88802
    Stopping after 500 packets
    Read 500 TS packets

The ``-cnt <pid>`` switch makes tsreport check
the values of the ``continuity_counter`` field for the specified PID.
It writes the values to a file called ``continuity_counter.txt``, in lines of
the form::

    0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15

which makes it easy to spot missing values. It also gives a warning
for any discontinuities found, but note that the specification does allow the
duplication of a TS packet once (which will lead to the continuity counter
repeating once as well).  Using ``-cnt`` automatically turns buffering
on (-b).


tsserve
=======
Acts as a server, playing PS or TS files to clients.

For instance::

    $ tsserve CharliesAngels.mpg

will listen for a client on the default port 88, and "serve" the file to it
when a connection is made.

Up to 10 files may be specified on the command line - for instance::

    $ tsserve  -0 CharliesAngels.mpg  -1 hp-trail.ts  -2 news24.ts

specifies files 0, 1 and 2. The first example, where no file number is
explicitly given, is equivalent to ``-0 CharliesAngels.mpg``.

If port 88 is not suitable, an alternative may be chosen::

    $ tsserve  -port 8889  -0 CharliesAngels.mpg  -1 hp-trail.ts  -2 news24.ts

The file being served starts in "p"ause mode. On reaching either end of the
file (the end by playing forwards at normal or accelerated speed, or the start
by rewinding), it returns to "p"ause mode.

A client may send single character commands to the server:

    * ``p`` - pause (the initial state)
    * ``n`` - normal play
    * ``f`` - fast forward (using "strip" as described in
      `Fast forward algorithms`_). The speed of this is dependent on the
      distribution of reference frames in the data.
    * ``F`` - fast fast forward (using "filter" as described in
      `Fast forward algorithms`_). The speed of this is nominally 8x.
    * ``r`` - reverse (as described in `Reverse algorithms`_). The nominal
      speed for this is 8x.
    * ``R`` - reverse at double speed, i.e., 16x.
    * ``>`` - skip forwards 10 seconds.
    * ``]`` - skip forwards 3 minutes.
    * ``<`` - skip backwards 10 seconds.
    * ``[`` - skip backwards 3 minutes.
    * ``0`` .. ``9`` - select the file with that number, and start playing
      it again from the start. If there is no file with that number, then
      ignore the command.
    * ``q`` - quit this client.

(typically by use of a handset).

For each client, tsserver spawns a new server (on Unix with ``fork``, on
Windows using a thread) which serves the nominated files to that client.
No particular limit is specified for the number of clients allowed.

When a client sends the ``q`` command, or if an error occurs, then the
particular process for that client will be terminated.

Notes
-----
Each file is output as a different TS program, file 0 as program 1, file 1 as
program 2, etc.

Different PIDs will be used for the data in each program. For program *i*,

* the video PID will be 0x68 + *i*,
* the audio PID will be 0x68 + *i* + 10, and
* the PMT PID will be 0x68 + *i* + 20.

The PCR PID will be assumed to be the video PID.

When reversing or fast forwarding MPEG-2 data, sequence headers will not,
by default be output (see `Reverse algorithms`_ for some background on this).
The ``-seqhdr`` switch may be used to override this.

Alternate modes
---------------
If only a specific host is to be used as a "client", then that host may be
named explicitly::

   $ tsserve -cmd -host norton  -0 CharliesAngels.mpg -1 hp-trail.ts

The ``-cmd`` indicates that the host may send single character commands (as
above).

It is also possible (on Unix, but not on Windows) to give commands to the
program directly, instead of via the client socket.

For instance::

    $ tsserve  -cmdstin  -host norton  -0 CharliesAngels.mpg -1 hp-trail.ts

The program will then read commands from standard input. Note, however, that
such commands will not be "seen" until a newline is typed (at which point any
commands typed will occur in the order given).

Some canned test modes are also supplied, which perform a reproducable
sequence of actions. These are described in the ``-details`` help text.

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
