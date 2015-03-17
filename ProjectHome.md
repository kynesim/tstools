This is a set of cross-platform command line tools for working with MPEG data.

The emphasis is on relatively simple tools which concentrate on MPEG (H.264 and H.262) data packaged according to H.222 (i.e., TS or PS), with a particular interest in checking for conformance.

Transport Stream (TS) is typically used for distribution of cable and satellite data. Program Stream (PS) is typically used to store data on DVDs.

The tools are focussed on:

  * Quick reporting of useful data (tsinfo, stream\_type)
  * Giving a quick overview of the entities in the stream (esdots, psdots)
  * Reporting on TS packets (tsreport) or ES units/frames/fields (esreport)
  * Simple manipulation of stream data (es2ts, esfilter, esreverse, esmerge, ts2es)
  * Streaming of data, possibly with introduced errors (tsplay)

There is an [Overview](http://code.google.com/p/tstools/wiki/Overview) of the tools on the wiki.

NB: This is the new home of the tstools project, previously at http://tstools.berlios.de/. Further development will occur here.

**As of 2012-03-15, tstools is now stored in git and not mercurial.** See the [Source](http://code.google.com/p/tstools/source/checkout) tab for how to clone the new repository.