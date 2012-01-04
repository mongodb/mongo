"""Builder for static libraries composed of the contents of other static libraries.

The following rule creates a library "mylib" whose contents are the contents of
"firstlib", "secondlib", and all LIBDEPS dependencies of "firstlib" and
"secondlib".  This creates self-contained static and shared libraries that can
be distributed to customers.

MergeLibrary('mylib', ['firstlib', 'secondlib'])
MergeSharedLibrary('mylib', ['firstlib', 'secondlib'])

This file provides the platform-independent tool, which just selects and imports
the platform-specific tool providing MergeLibrary and MergeSharedLibrary.
"""

import sys

def exists( env ):
    return True

def generate( env ):
    if sys.platform == 'win32':
        env.Tool( 'mergelibwin' )
    else:
        env.Tool( 'mergelibposix' )
