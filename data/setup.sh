#! /bin/sh
#
# A very simple script to retrieve and pre-process test data, as used by
# ../python/rundoctest.py
#
# Assumes it is being run in the 'data' directory
# Assumes the presence of 'wget' and 'unzip', and that the tstools have been
# built.

# Retrieve a segment of Elephant's Dream in TS (11 is the smallest segment)
wget http://www.w6rz.net/ed24p_11.zip
unzip ed24p_11.zip
ts2es ed24p_11.ts ed24p_11.video.es

# Afterwards:
#
#  ed24p_11.zip                 146M
#  ed26p_11.ts                  314M
#  ed24p_11.video.es            147M

