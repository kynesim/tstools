Test data
=========
It can be difficult to find test data, particularly for Transport Stream.

It is especially difficult to work out what one is allowed to copy and put on
one's own website.

The doctests in the "python" directory (sibling to this data directory) need
at least *some* predictable data to play with.

Contents of this directory
==========================
For Linux users, a convenient ``setup.sh`` is provided, which will use
``wget`` to download a segment of `Elephant's Dream`_, ``unzip`` it, and
produce an ES file from (the resulting) TS file.

This is then used as test data by the doctests in the sibling ``python``
directory.

Useful links
============

Consolidated list of test video clip resources
----------------------------------------------
http://forum.doom9.org/archive/index.php/t-135034.html

A hopefully usedul resource on the Doom9 forum.

Note the link to a list of "broken" streams:

  http://forum.doom9.org/showthread.php?t=134693

Elephant's Dream
----------------
http://www.elephantsdream.org/

Last checked: 2008-08-29

"""Elephants Dream is the world’s first open movie, made entirely with open
source graphics software such as Blender, and with all production files freely
available to use however you please, under a Creative Commons license."""

It is thus clearly allowable to use this data for testing purposes -- see
below.

W6RZ Homepage -- MPEG-2 Transport Stream Test Patterns and Tools
----------------------------------------------------------------
http://www.w6rz.net/

Last checked: 2008-08-29

This appears to be a very useful resource. It includes a variety of HD
Transport Stream samples, including segments of a TS version of Elephant's
Dream.

There is a discussion about the site at
http://www.avsforum.com/avs-vb/archive/index.php/t-570937.html
(posts date from 2005 to 2008-11-08, as I'm typing this).

MPEG 1 layer 1/2/3 MPEG 2 2/3 Test Data   at mpegedit.org
---------------------------------------------------------
http://mpgedit.org/mpgedit/mpgedit/testdata/mpegdata.html

Last checked: 2008-08-29 (I've not tried any of the data yet, though))

I found this by following a link from
http://www.mpeg.org/MPEG/mpeg-systems-resources-and-software/mpeg-systems-test-bitstreams.html

ftp://vqeg.its.bldrdoc.gov/HDTV/SVT_exports/
-------------------------------------------
This site is referenced from the W6RZ site, as the source of the "Park Run"
clip (a version of which is on the W6RZ site).

Note that the README.txt at this location puts restrictions on the use of the
data on this site, specifically:

"""Individuals and organizations extracting sequences from this archive agree
that the sequences and all intellectual property rights therein remain the
property of Sveriges Television AB (SVT), Sweden. These sequences may only be
used for the purpose of developing, testing and presenting technology
standards. SVT makes no warranties with respect to the materials and expressly
disclaim any warranties regarding their fitness for any purpose."""

The PDF svt_widexga_final.pdf appears to be an interesting paper, and gives
previews of each clip.
