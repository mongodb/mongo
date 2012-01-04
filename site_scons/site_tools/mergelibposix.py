"""Builder for static libraries composed of the contents of other static libraries.

The following rule creates a library "mylib" whose contents are the contents of
"firstlib", "secondlib", and all LIBDEPS dependencies of "firstlib" and
"secondlib".  This creates self-contained static and shared libraries that can
be distributed to customers.

MergeLibrary('mylib', ['firstlib', 'secondlib'])
MergeSharedLibrary('mylib', ['firstlib', 'secondlib'])

This implementation is for posix systems whose linkers can generate "relocatable
objects" (usually with the -r option).
"""

import libdeps
from SCons.Action import Action
from SCons.Builder import Builder

def merge_library_method( env, target, source, LIBDEPS=None, **kwargs ):
    robj_name = env.subst( '${TARGET}-mergelib', target=target, source=source )
    robj = env.RelocatableObject( robj_name, [], LIBDEPS=source, **kwargs )
    return env.Library( target, robj, LIBDEPS=LIBDEPS or [], **kwargs )

def merge_shared_library_method( env, target, source, LIBDEPS=None, **kwargs ):
    robj_name = env.subst( '${TARGET}-mergeshlib', target=target, source=source )
    robj = env.RelocatableObject( robj_name, [], LIBDEPS=source, **kwargs )
    return env.SharedLibrary( target, robj, LIBDEPS=LIBDEPS or [], **kwargs )

def exists( env ):
    return True

def generate( env ):
    env['_RELOBJDEPSFLAGS'] = '$RELOBJ_LIBDEPS_START ${_concat("$RELOBJ_LIBDEPS_ITEM ", __env__.subst(_LIBDEPS, target=TARGET, source=SOURCE), "", __env__, target=TARGET, source=SOURCE)} $RELOBJ_LIBDEPS_END'
    env['RELOBJCOM'] = 'ld -o $TARGET -r $SOURCES $_RELOBJDEPSFLAGS'
    relobj_builder = Builder( action='$RELOBJCOM',
                              prefix="$OBJPREFIX",
                              suffix="$OBJSUFFIX",
                              emitter=libdeps.libdeps_emitter )
    libdeps.update_scanner( relobj_builder )
    env['BUILDERS']['RelocatableObject'] = relobj_builder
    env.AddMethod( merge_library_method, 'MergeLibrary' )
    env.AddMethod( merge_shared_library_method, 'MergeSharedLibrary' )
