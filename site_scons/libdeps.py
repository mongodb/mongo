"""Extension to SCons providing advanced static library dependency tracking.

These modifications to a build environment, which can be attached to
StaticLibrary and Program builders via a call to setup_environment(env),
cause the build system to track library dependencies through static libraries,
and to add them to the link command executed when building programs.

For example, consider a program 'try' that depends on a lib 'tc', which in
turn uses a symbol from a lib 'tb' which in turn uses a library from 'ta'.
Without this package, the Program declaration for "try" looks like this:

Program('try', ['try.c', 'path/to/${LIBPREFIX}tc${LIBSUFFIX}',
                'path/to/${LIBPREFIX}tc${LIBSUFFIX}',
                'path/to/${LIBPREFIX}tc${LIBSUFFIX}',])

With this library, we can instead write the following

Program('try', ['try.c'], LIBDEPS=['path/to/tc'])
StaticLibrary('tc', ['c.c'], LIBDEPS=['path/to/tb'])
StaticLibrary('tb', ['b.c'], LIBDEPS=['path/to/ta'])
StaticLibrary('ta', ['a.c'])

And the build system will figure out that it needs to link libta.a and libtb.a
when building 'try'.
"""

# Copyright (c) 2010, Corensic Inc., All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

import os

import SCons.Errors
import SCons.Scanner
import SCons.Util

def sorted_by_str(iterable):
    """Shorthand for sorting an iterable according to its string representation.

    We use this instead of sorted(), below, because SCons.Node objects are
    compared on object identity, rather than value, so sorts aren't stable
    across invocations of SCons.  Since dependency order changes force rebuilds,
    we use this sort to create stable dependency orders.
    """
    return sorted(iterable, cmp=lambda lhs, rhs: cmp(str(lhs), str(rhs)))

class DependencyCycleError(SCons.Errors.UserError):
    """Exception representing a cycle discovered in library dependencies."""

    def __init__(self, first_node ):
        super(DependencyCycleError, self).__init__()
        self.cycle_nodes = [first_node]

    def __str__(self):
        return " => ".join(str(n) for n in self.cycle_nodes)

def __get_libdeps(node, env_var):
    """Given a SCons Node, return its library dependencies.

    Computes the dependencies if they're not already cached.
    """

    cached_var_name = env_var + '_cached'

    if not hasattr(node.attributes, cached_var_name):
        setattr(node.attributes, cached_var_name, __compute_libdeps(node, env_var))
    return getattr(node.attributes, cached_var_name)

def __compute_libdeps(node, env_var):
    """Recursively identify all library dependencies for a node."""

    if getattr(node.attributes, 'libdeps_exploring', False):
        raise DependencyCycleError(node)

    env = node.get_env()
    deps = set()
    node.attributes.libdeps_exploring = True
    try:
        try:
            for child in env.Flatten(env.get(env_var, [])):
                if not child:
                    continue
                deps.add(child)
                deps.update(__get_libdeps(child, env_var))

        except DependencyCycleError, e:
            if len(e.cycle_nodes) == 1 or e.cycle_nodes[0] != e.cycle_nodes[-1]:
                e.cycle_nodes.append(node)
                raise
    finally:
        node.attributes.libdeps_exploring = False

    return deps

def update_scanner(builder):
    """Update the scanner for "builder" to also scan library dependencies."""

    old_scanner = builder.target_scanner

    if old_scanner:
        path_function = old_scanner.path_function
        def new_scanner(node, env, path=()):
            result = set(old_scanner.function(node, env, path))
            result.update(__get_libdeps(node, 'LIBDEPS'))
            result.update(__get_libdeps(node, 'SYSLIBDEPS'))
            return sorted_by_str(result)
    else:
        path_function = None
        def new_scanner(node, env, path=()):
            result = set(__get_libdeps(node, 'LIBDEPS'))
            result.update(__get_libdeps(node, 'SYSLIBDEPS'))
            return sorted_by_str(result)

    builder.target_scanner = SCons.Scanner.Scanner(function=new_scanner,
                                                    path_function=path_function)

def get_libdeps(source, target, env, for_signature):
    """Implementation of the special _LIBDEPS environment variable.

    Expands to the library dependencies for a target.
    """

    if for_signature:
        return []
    target = env.Flatten([target])
    return list(__get_libdeps(target[0], 'LIBDEPS'))

def get_libdeps_objs(source, target, env, for_signature):
    if for_signature:
        return []

    objs = set()
    for lib in get_libdeps(source, target, env, for_signature):
        objs.update(lib.sources_set)
    return list(objs)

def get_libdeps_special_sun(source, target, env, for_signature):
    if for_signature:
        return []

    x = get_libdeps(source, target, env, for_signature )
    return x + x + x

def get_syslibdeps(source, target, env, for_signature):
    if for_signature:
        return[]
    deps = list(__get_libdeps(target[0], 'SYSLIBDEPS'))
    return deps

def libdeps_emitter(target, source, env):
    """SCons emitter that takes values from the LIBDEPS environment variable and
    converts them to File node objects, binding correct path information into
    those File objects.

    Emitters run on a particular "target" node during the initial execution of
    the SConscript file, rather than during the later build phase.  When they
    run, the "env" environment's working directory information is what you
    expect it to be -- that is, the working directory is considered to be the
    one that contains the SConscript file.  This allows specification of
    relative paths to LIBDEPS elements.

    This emitter also adds LIBSUFFIX and LIBPREFIX appropriately.
    """

    libdep_files = []
    lib_suffix = env.subst('$LIBSUFFIX', target=target, source=source)
    lib_prefix = env.subst('$LIBPREFIX', target=target, source=source)
    for dep in env.Flatten([env.get('LIBDEPS', [])]):
        full_path = env.subst(str(dep), target=target, source=source)
        dir_name = os.path.dirname(full_path)
        file_name = os.path.basename(full_path)
        if not file_name.startswith(lib_prefix):
            file_name = '${LIBPREFIX}' + file_name
        if not file_name.endswith(lib_suffix):
            file_name += '${LIBSUFFIX}'
        libdep_files.append(env.File(os.path.join(dir_name, file_name)))

    env['LIBDEPS'] = libdep_files

    return target, source

def setup_environment(env):
    """Set up the given build environment to do LIBDEPS tracking."""

    try:
        env['_LIBDEPS']
    except KeyError:
        env['_LIBDEPS'] = '$_LIBDEPS_LIBS'

    # TODO: remove this
    # this is a horrible horrible hack for 
    # for 32-bit solaris
    if "uname" in dir(os) and os.uname()[1] == "sun32b":
        env['_LIBDEPS_LIBS'] = get_libdeps_special_sun
    else:
        env['_LIBDEPS_LIBS'] = get_libdeps

    env['_LIBDEPS_OBJS'] = get_libdeps_objs
    env['_SYSLIBDEPS'] = ' ${_stripixes(LIBLINKPREFIX, SYSLIBDEPS, LIBLINKSUFFIX, LIBPREFIXES, LIBSUFFIXES, __env__)} '
    env['_SHLIBDEPS'] = '$SHLIBDEP_GROUP_START ${_concat(SHLIBDEPPREFIX, __env__.subst(_LIBDEPS, target=TARGET, source=SOURCE), SHLIBDEPSUFFIX, __env__, target=TARGET, source=SOURCE)} $SHLIBDEP_GROUP_END'

    env['LIBDEPS'] = SCons.Util.CLVar()
    env['SYSLIBDEPS'] = SCons.Util.CLVar()
    env.Append(LIBEMITTER=libdeps_emitter,
               PROGEMITTER=libdeps_emitter,
               SHLIBEMITTER=libdeps_emitter)
    env.Prepend(_LIBFLAGS=' $LINK_LIBGROUP_START $_LIBDEPS $LINK_LIBGROUP_END $_SYSLIBDEPS ')
    for builder_name in ('Program', 'SharedLibrary', 'LoadableModule'):
        try:
            update_scanner(env['BUILDERS'][builder_name])
        except KeyError:
            pass
