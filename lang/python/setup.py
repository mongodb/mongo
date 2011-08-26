import re, os
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
	os.environ['ARCHFLAGS'] = ''

dir = os.path.dirname(__file__)

# Read the version information from dist/RELEASE
dist = os.path.join(os.path.dirname(os.path.dirname(dir)), 'dist')
for l in open(os.path.join(dist, 'RELEASE')):
	if re.match(r'WIREDTIGER_VERSION_(?:MAJOR|MINOR|PATCH)=', l):
		exec(l)

wt_ver = '%d.%d' % (WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR)

setup(name='wiredtiger', version=wt_ver,
    ext_modules=[Extension('_wiredtiger',
		[os.path.join(dir, 'wiredtiger_wrap.c')],
	    include_dirs=['.'],
	    library_dirs=['.libs'],
	    libraries=['wiredtiger'],
	)],
	py_modules=['wiredtiger'],
)
