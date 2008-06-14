=====
To do
=====

.. notes: Check that all outstanding items in this file are still outstanding.


Bugs:

* esreverse (in particular, but really all the filtering programs) should not
  abort if an error occurs reading through the file - in esreverse's case,
  this causes no output to be produced, which is irritating.

* When reading PS data, if the next packet does not start with 00 00 01,
  then a search will be made for the next pack header (specifically, this
  is done by ``read_PS_packet_start``).

  This means that a file with "broken" data in its middle can be coped
  with by the PS reader - it will skip when it fails to find the next
  00 00 01 in the right place.

  It does not mean that other layers above the PS reader will cope with
  broken data neatly, in particular if said "breakage" happened inside the
  last PS packet read before the skip. Ideally, these other levels would
  also have a strategy for moving on to the next believed-good point
  (perhaps even the next PS pack header).

      NB: TS data is expected not to contain errors in this sense.

Outstanding items:

* Sort out filter for H.264 so that it works sensibly.
  Generally check that H.264 works correctly with tsserve - there are
  believed to be known transition issues still outstanding.

  In more detail:

    1. When stopping filtering, go back in the reverse list to the previous
       IDR and output that before continuing. This is needed to stop things
       referring to non-existent reference frames.

       Note that this might be a good thing to do cosmetically when stripping
       as well, but since tsserve outputs all reference pictures when
       stripping, it is not *necessary*.
       
    2. When reversing, an IDR must also be output, so that any P/B frames
       thereafter are known not to be referring "past" it. This can be done
       either by insisting that reversing stop on an IDR, or else by
       reading forwards and outputting I and IDR frames until an IDR is found.

   This adds a requirement to be able to identify IDR frames in the reverse
   list.

* Worry about AFD values and sequence headers in H.262.

    *Believed DONE, except for the "read and remember the GOP" bits, which
    may or may not matter.*

  Note that if sequence headers are not output when F or FF is used (the
  default, currently), then using F or FF as soon as tsserve is listening
  will not display any pictures, until N is selected (at least on norton),
  because nothing is displayed before a sequence header.

    The work needed to make reversing work "properly" for H.262.

    For H.264 it is sufficient to output just the block of data for a picture
    when reversing. However, for H.262 it is clearly not that simple, if only
    because of the AFD problem. What is probably wanted for H.262 is something
    more like:

      * Remember the start of a frame
      * Remember the start of "relevant" sequence headers
      * Remember (as now) which sequence header that "corresponds" to a frame
      * Remember the AFD (it's only 8 bytes, and not all of that necessarily
        is needed) for each frame
      * Maybe also remember the start of GOP headers, and their correspondences

        (NB: remember to cope with data that doesn't have AFDs?)

    To *output* a frame then means:

      a. [Locate its GOP and output that]
      b. Locate its sequence header, and output that
      c. Move to the start of the frame, and read and output its picture header
      d. Output the AFD
      e. Read and output the rest of the frame

    Simple optimisations along the lines of "if the GOP/sequence header have
    not changed then don't bother to re-output them" and "if no sequence
    header has been output then don't output an AFD" can be added when the
    approach seems to be working.

    Refreshing my memory - the sequence of H.262 data is::

        for 1..n:
            Seq header
            Seq extension
            for 0..n:
                Extension & user data
            for 1..n:
                optional:
                    GOP header
                    for 0..n: User data
                Picture header
                Picture coding extension
                for 0..n:
                    Extension & user data  # including AFD
                Picture data...
        Seq end
    
    MPEG-1 is slightly simpler, and goes something like::

        for 1..n:
            Seq header
            for 0..n:
                Extension & user data
            for 1..n:
                optional:
                    GOP header
                    for 0..n: User data
                Picture header
                for 0..n:
                    Extension & user data  # including AFD
                Picture data...
        Seq end

* PS reading may still be slow.

* Continue to use valgrind and its cohorts to check for leaks and
  inefficiencies.


----------------------------------------

Other outstanding items, in no particular order. Some of these may never
actually be worth the time to do.

* Overhaul the older code to bring it up to the style of tsserve.

* Locate and check all ``@@@`` comments in the code.

* Consider moving the verbose and quiet flags into the various context
  entities, allowing closer control on exactly *what* is quiet/verbose.
  This should allow replacement of debug conditions in individual files
  with equivalent flags in the appropriate context, and closer control
  of what output is available for debugging.

* Consider leaving a gap between PAT and PMT, since some clients read the
  program stream and program metadata in parallel, so may take a while to
  "notice" that they should be using a new PMT.

      This actually sounds like a good idea.

* Maybe remove the (tail) recursion from write_some_TS_PES_packet (except that
  it doesn't appear to impact efficiency at all, so it can wait).

* When outputting a picture (access unit), consider outputting the whole
  thing as one (TS) PES packet, rather than outputting each item/NAL unit
  as a PES packet.

* Regularise/make sensible the units used for -max in all utilities
  (some are still working on NAL units/H.262 items when they should
  be using pictures?).

* Make verbose and quiet be in the same order in all functions using them(!)

* Write more documentation.

* Note which functions are (only used as) LIBRARY (functions), and which
  "truly" EXTERN.

* Check all help texts.

  In particular, check what tsplay does for its timing info, and ensure its
  help text is correct. Also, check what ps2ts does re timing, vs what it says
  in its help text.

* PS from DVD has an extra field (navigation info?). A user has said that
  DVDAuthor won't write PS to DVD without it containing said field. Consider a
  small utility to read PS and write PS with said field (as a dummy) added in.

* Report __LINE__ and __FILE__ in internal errors that "should not happen"???

* Regularise meaning and behaviour of -verbose, -quiet and -x (should that
  last be called thus, or something more like -debug?). Regularise if -verbose
  implies not -quiet, and vice versa, in particular.

* Make all the messages output that currently say "picture" or "access unit"
  that *should* say "frame" actually say "frame".

* In pes.c, should it allow such flexibility in choice of which PIDs to
  aggregate (or just keep) PES data for?

  If the user had to select video/audio PIDs up-front, and no other PES
  packets were kept, then it could get away with reusing the PES packet
  data for each (just resetting the innards, and reallocing the data buffer
  if it was not big enough).

  Would this save enough time to be worthwhile?

* Similarly, the current codebase is profligate in its use of malloc/free,
  because of the way it handles all the "context" entities. It would be
  possible to use more local variables for many of these (although still
  at the cost of remembering to call setup/teardown functions on them).
  Given this, some variables could (probably) be reused instead of
  building new instances and then freeing them.

  Again, would this save enough time to be worthwhile?

* Missing programs?

    - ts2ps -- a preliminary implementation exists (build it explicitly with
      ``make ts2ps``), but it generates incorrect data.
    - es2ps -- is this ever needed?
    - ps2es -- ``ts2es -pes`` should actually do this

    - tsdots -- would be useful sometimes

.. ***** BEGIN LICENSE BLOCK *****

License
-------
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
