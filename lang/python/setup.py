import os
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
	os.environ['ARCHFLAGS'] = ''

setup(name='wiredtiger', version='1.0',
    ext_modules=[Extension('_wiredtiger', ['wiredtiger.i'],
	    swig_opts=['-I../../build_posix'],
	    include_dirs=['../../build_posix'],
	    library_dirs=['../../build_posix'],
	    libraries=['wiredtiger'],
	)],
	py_modules=['wiredtiger'],
)
