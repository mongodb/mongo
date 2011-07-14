import os
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
	os.environ['ARCHFLAGS'] = ''

dir = os.path.dirname(__file__)

extra_compile_args = None
if 'CFLAGS' in os.environ:
    extra_compile_args = [os.environ['CFLAGS']]
extra_link_args = None
if 'LDFLAGS' in os.environ:
    extra_link_args = [os.environ['LDFLAGS']]

setup(name='wiredtiger', version='1.0',
    ext_modules=[Extension('_wiredtiger',
		[os.path.join(dir, 'wiredtiger_wrap.c')],
	    include_dirs=['.'],
	    library_dirs=['.'],
	    libraries=['wiredtiger'],
		extra_compile_args=extra_compile_args,
		extra_link_args=extra_link_args,
	)],
	py_modules=['wiredtiger'],
)
