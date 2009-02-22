# This is a preliminary Makefile for experimentin with Pyrex builds
# It assumes that the normal tstools Makefile has been used to build
# the tools already. It also assumes that Pyrex is installed, and that
# pyrexc is on the path. It probably assumes Linux or Mac OS/X as well...

# Note: Building tstools.pyx needs at least Pyrex 0.9.7 (because I use the new
# 'for' loop syntax, apart from anything else). On Ubuntu, this means
# installing it "by hand", as the version packaged for Hardy Heron (as of
# 27-Aug-2008, at least) is 0.9.6.4

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

.PHONY: all setup test
all:	setup test

.PHONY: setup
setup:
	python Setup.py build_ext --inplace

.PHONY: test
test:	setup
	./rundoctest.py

.PHONY: clean
clean:
	python Setup.py clean
	-rm tstools/*.so

.PHONY: distclean
distclean: clean
	-rm tstools/*.c
