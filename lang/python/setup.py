import os, sys
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
	os.environ['ARCHFLAGS'] = ''
dir = os.path.dirname(sys.argv[0])

setup(name='wiredtiger', version='1.0',
    ext_modules=[Extension('_wiredtiger', [os.path.join(dir, 'wiredtiger.i')],
	    include_dirs=['.'],
	    library_dirs=['.'],
	    libraries=['wiredtiger'],
	)],
	py_modules=['wiredtiger'],
)
