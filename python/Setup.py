"""Setup.py -- for building tstools Pyrex modules
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
#   Tibs (tibs@berlios.de)
# 
# ***** END LICENSE BLOCK *****

from distutils.core import setup
from distutils.extension import Extension
from Cython.Distutils import build_ext

tstools = Extension("tstools.tstools",
                    ['tstools/tstools.pyx'],
                    include_dirs=['..'],
                    library_dirs=['../lib'],
                    libraries=['tstools'],
                    )
setup(
  name = 'tstools',
  cmdclass = {'build_ext': build_ext},
  ext_modules=[tstools]
)
