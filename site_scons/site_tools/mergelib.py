"""Builder for static libraries composed of the contents of other static libraries.

The following rule creates a library "mylib" whose contents are the contents of
"firstlib", "secondlib", and all LIBDEPS dependencies of "firstlib" and
"secondlib".  This creates self-contained static and shared libraries that can
be distributed to customers.

MergeLibrary('mylib', ['firstlib', 'secondlib'])

"""

import libdeps
from SCons.Action import Action
from SCons.Builder import Builder

def merge_library_method(env, target, source, LIBDEPS=None, **kwargs):
    return env._MergeLibrary(target, [], LIBDEPS=source, **kwargs)

def exists( env ):
    return True

def generate( env ):
    merge_library = Builder(
        action='$ARCOM $_LIBDEPS_OBJS',
        src_prefix='$LIBPREFIX',
        src_suffix='$LIBSUFFIX',
        prefix='$LIBPREFIX',
        suffix='$LIBSUFFIX',
        emitter=libdeps.libdeps_emitter )
    libdeps.update_scanner( merge_library )
    env['BUILDERS']['_MergeLibrary'] = merge_library
    env.AddMethod( merge_library_method, 'MergeLibrary' )
