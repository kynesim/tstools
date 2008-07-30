"""Setup.py -- for building tstools Pyrex modules
"""
from distutils.core import setup
from Pyrex.Distutils.extension import Extension
from Pyrex.Distutils import build_ext

setup(
  name = 'tstools',
  ext_modules=[ 
    Extension("tstools", ["tstools.pyx"],
              libraries=['tstools'],
              library_dirs=['lib'],
              ),
    ],
  cmdclass = {'build_ext': build_ext}
)

