#! /usr/bin/env python
"""Build HTML from the reStructuredText files in this directory.

This is a script just so I don't have to remember the particular incantation
required. It's not in the Makefile because I'm not yet sure it belongs there...

Requires Python and docutils.

Uses rst2html.py on individual files because that seems to be available
more often than the buildhtml.py script.
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
#   Amino Communications Ltd, Swavesey, Cambridge UK
# 
# ***** END LICENSE BLOCK *****

import os

def main():
    filenames = os.listdir('.')
    for name in filenames:
        base,ext = os.path.splitext(name)
        if ext == '.txt':
            print 'Processing',name
            os.system('rst2html --stylesheet-path=default.css'
                      ' --embed-stylesheet %s > %s'%(name,base+'.html'))


if __name__ == "__main__":
    main()
