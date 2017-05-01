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

A StaticLibrary S may also declare programs or libraries, [L1, ...] to be dependent
upon S by setting LIBDEPS_DEPENDENTS=[L1, ...], using the same syntax as is used
for LIBDEPS, except that the libraries and programs will not have LIBPREFIX/LIBSUFFIX
automatically added when missing.
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

libdeps_env_var = 'LIBDEPS'
syslibdeps_env_var = 'SYSLIBDEPS'
missing_syslibdep = 'MISSING_LIBDEP_'

class dependency(object):
    Public, Private, Interface = range(3)

    def __init__(self, value, dynamic):
        if isinstance(value, tuple):
            self.target_node = value[0]
            # All dependencies are public if we are not in dynamic mode.
            self.dependency_type = value[1] if dynamic else dependency.Public
        else:
            # Dependency edges are public by default
            self.target_node = value
            self.dependency_type = dependency.Public

class DependencyCycleError(SCons.Errors.UserError):
    """Exception representing a cycle discovered in library dependencies."""

    def __init__(self, first_node ):
        super(DependencyCycleError, self).__init__()
        self.cycle_nodes = [first_node]

    def __str__(self):
        return "Library dependency cycle detected: " + " => ".join(str(n) for n in self.cycle_nodes)

def __get_sorted_direct_libdeps(node):
    direct_sorted = getattr(node.attributes, "libdeps_direct_sorted", False)
    if not direct_sorted:
        direct = getattr(node.attributes, 'libdeps_direct', [])
        direct_sorted = sorted(direct, key=lambda t: str(t.target_node))
        setattr(node.attributes, "libdeps_direct_sorted", direct_sorted)
    return direct_sorted

def __get_libdeps(node):

    """Given a SCons Node, return its library dependencies, topologically sorted.

    Computes the dependencies if they're not already cached.
    """

    cached_var_name = libdeps_env_var + '_cached'

    if hasattr(node.attributes, cached_var_name):
        return getattr(node.attributes, cached_var_name)

    tsorted = []
    marked = set()

    def visit(n):
        if getattr(n.target_node.attributes, 'libdeps_exploring', False):
            raise DependencyCycleError(n.target_node)

        n.target_node.attributes.libdeps_exploring = True
        try:

            if n.target_node in marked:
                return

            try:
                for child in __get_sorted_direct_libdeps(n.target_node):
                    if child.dependency_type != dependency.Private:
                        visit(child)

                marked.add(n.target_node)
                tsorted.append(n.target_node)

            except DependencyCycleError, e:
                if len(e.cycle_nodes) == 1 or e.cycle_nodes[0] != e.cycle_nodes[-1]:
                    e.cycle_nodes.insert(0, n.target_node)
                raise

        finally:
            n.target_node.attributes.libdeps_exploring = False

    for child in __get_sorted_direct_libdeps(node):
        if child.dependency_type != dependency.Interface:
            visit(child)

    tsorted.reverse()
    setattr(node.attributes, cached_var_name, tsorted)

    return tsorted

def __get_syslibdeps(node):
    """ Given a SCons Node, return its system library dependencies.

    These are the depencencies listed with SYSLIBDEPS, and are linked using -l.
    """
    cached_var_name = syslibdeps_env_var + '_cached'
    if not hasattr(node.attributes, cached_var_name):
        syslibdeps = node.get_env().Flatten(node.get_env().get(syslibdeps_env_var, []))
        for lib in __get_libdeps(node):
            for syslib in node.get_env().Flatten(lib.get_env().get(syslibdeps_env_var, [])):
                if syslib:
                    if type(syslib) in (str, unicode) and syslib.startswith(missing_syslibdep):
                        print("Target '%s' depends on the availability of a "
                              "system provided library for '%s', "
                              "but no suitable library was found during configuration." %
                              (str(node), syslib[len(missing_syslibdep):]))
                        node.get_env().Exit(1)
                    syslibdeps.append(syslib)
        setattr(node.attributes, cached_var_name, syslibdeps)
    return getattr(node.attributes, cached_var_name)

def __missing_syslib(name):
    return missing_syslibdep + name

def update_scanner(builder):
    """Update the scanner for "builder" to also scan library dependencies."""

    old_scanner = builder.target_scanner

    if old_scanner:
        path_function = old_scanner.path_function
        def new_scanner(node, env, path=()):
            result = old_scanner.function(node, env, path)
            result.extend(__get_libdeps(node))
            return result
    else:
        path_function = None
        def new_scanner(node, env, path=()):
            return __get_libdeps(node)

    builder.target_scanner = SCons.Scanner.Scanner(function=new_scanner,
                                                    path_function=path_function)

def get_libdeps(source, target, env, for_signature):
    """Implementation of the special _LIBDEPS environment variable.

    Expands to the library dependencies for a target.
    """

    target = env.Flatten([target])
    return __get_libdeps(target[0])

def get_libdeps_objs(source, target, env, for_signature):
    objs = []
    for lib in get_libdeps(source, target, env, for_signature):
        # This relies on Node.sources being order stable build-to-build.
        objs.extend(lib.sources)
    return objs

def get_syslibdeps(source, target, env, for_signature):
    deps = __get_syslibdeps(target[0])
    lib_link_prefix = env.subst('$LIBLINKPREFIX')
    lib_link_suffix = env.subst('$LIBLINKSUFFIX')
    result = []
    for d in deps:
        # Elements of syslibdeps are either strings (str or unicode), or they're File objects.
        # If they're File objects, they can be passed straight through.  If they're strings,
        # they're believed to represent library short names, that should be prefixed with -l
        # or the compiler-specific equivalent.  I.e., 'm' becomes '-lm', but 'File("m.a") is passed
        # through whole cloth.
        if type(d) in (str, unicode):
            result.append('%s%s%s' % (lib_link_prefix, d, lib_link_suffix))
        else:
            result.append(d)
    return result

def __append_direct_libdeps(node, prereq_nodes):
    # We do not bother to decorate nodes that are not actual Objects
    if type(node) == str:
        return
    if getattr(node.attributes, 'libdeps_direct', None) is None:
        node.attributes.libdeps_direct = []
    node.attributes.libdeps_direct.extend(prereq_nodes)

def __normalize_libdeps(libdeps, dynamic):
    """Promote all entries in the libdeps list to the dependency type"""
    return [dependency(l, dynamic) for l in libdeps if l is not None]

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

    NOTE: For purposes of LIBDEPS_DEPENDENTS propagation, only the first member
    of the "target" list is made a prerequisite of the elements of LIBDEPS_DEPENDENTS.
    """

    lib_builder = env['BUILDERS']['StaticLibrary']
    lib_node_factory = lib_builder.target_factory or env.File

    prog_builder = env['BUILDERS']['Program']
    prog_node_factory = prog_builder.target_factory or env.File

    prereqs = __normalize_libdeps(env.get(libdeps_env_var, []), dynamic=False)
    for prereq in prereqs:
        prereqWithIxes = SCons.Util.adjustixes(
            prereq.target_node, lib_builder.get_prefix(env), lib_builder.get_suffix(env))
        prereq.target_node = lib_node_factory(prereqWithIxes)

    for t in target:
        # target[0] must be a Node and not a string, or else libdeps will fail to
        # work properly.
        __append_direct_libdeps(t, prereqs)

    for dependent in env.get('LIBDEPS_DEPENDENTS', []):
        if dependent is None:
            continue
        dependentWithIxes = SCons.Util.adjustixes(
            dependent, lib_builder.get_prefix(env), lib_builder.get_suffix(env))
        dependentNode = lib_node_factory(dependentWithIxes)
        __append_direct_libdeps(dependentNode, [dependency(target[0], dependency.Public)])

    for dependent in env.get('PROGDEPS_DEPENDENTS', []):
        if dependent is None:
            continue
        dependentWithIxes = SCons.Util.adjustixes(
            dependent, prog_builder.get_prefix(env), prog_builder.get_suffix(env))
        dependentNode = prog_node_factory(dependentWithIxes)
        __append_direct_libdeps(dependentNode, [dependency(target[0], dependency.Public)])

    return target, source

def shlibdeps_emitter(target, source, env):
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

    NOTE: For purposes of LIBDEPS_DEPENDENTS propagation, only the first member
    of the "target" list is made a prerequisite of the elements of LIBDEPS_DEPENDENTS.
    """

    lib_builder = env['BUILDERS']['SharedLibrary']
    lib_node_factory = lib_builder.target_factory or env.File

    prog_builder = env['BUILDERS']['Program']
    prog_node_factory = prog_builder.target_factory or env.File

    prereqs = __normalize_libdeps(env.get(libdeps_env_var, []), dynamic=True)
    for prereq in prereqs:
        prereqWithIxes = SCons.Util.adjustixes(
            prereq.target_node, lib_builder.get_prefix(env), lib_builder.get_suffix(env))
        prereq.target_node = lib_node_factory(prereqWithIxes)

    for t in target:
        # target[0] must be a Node and not a string, or else libdeps will fail to
        # work properly.
        __append_direct_libdeps(t, prereqs)

    for dependent in env.get('LIBDEPS_DEPENDENTS', []):
        if dependent is None:
            continue
        dependentWithIxes = SCons.Util.adjustixes(
            dependent, lib_builder.get_prefix(env), lib_builder.get_suffix(env))
        dependentNode = lib_node_factory(dependentWithIxes)
        __append_direct_libdeps(dependentNode, [dependency(target[0], dependency.Private)])

    for dependent in env.get('PROGDEPS_DEPENDENTS', []):
        if dependent is None:
            continue
        dependentWithIxes = SCons.Util.adjustixes(
            dependent, prog_builder.get_prefix(env), prog_builder.get_suffix(env))
        dependentNode = prog_node_factory(dependentWithIxes)
        __append_direct_libdeps(dependentNode, [dependency(target[0], dependency.Private)])

    return target, source

def expand_libdeps_tags(source, target, env, for_signature):
    results = []
    for expansion in env.get('LIBDEPS_TAG_EXPANSIONS', []):
        results.append(expansion(source, target, env, for_signature))
    return results

def setup_environment(env, emitting_shared=False):
    """Set up the given build environment to do LIBDEPS tracking."""

    try:
        env['_LIBDEPS']
    except KeyError:
        env['_LIBDEPS'] = '$_LIBDEPS_LIBS'

    env['_LIBDEPS_TAGS'] = expand_libdeps_tags
    env['_LIBDEPS_GET_LIBS'] = get_libdeps
    env['_LIBDEPS_OBJS'] = get_libdeps_objs
    env['_SYSLIBDEPS'] = get_syslibdeps

    env[libdeps_env_var] = SCons.Util.CLVar()
    env[syslibdeps_env_var] = SCons.Util.CLVar()

    env.Append(LIBEMITTER=libdeps_emitter)
    if emitting_shared:
        env['_LIBDEPS_LIBS'] = '$_LIBDEPS_GET_LIBS'
        env.Append(
            PROGEMITTER=shlibdeps_emitter,
            SHLIBEMITTER=shlibdeps_emitter)
    else:

        def expand_libdeps_with_extraction_flags(source, target, env, for_signature):
            result = []
            libs = get_libdeps(source, target, env, for_signature)
            for lib in libs:
                if 'init-no-global-side-effects' in env.Entry(lib).get_env().get('LIBDEPS_TAGS', []):
                    result.append(str(lib))
                else:
                    result.extend(env.subst('$LINK_WHOLE_ARCHIVE_LIB_START'
                                            '$TARGET'
                                            '$LINK_WHOLE_ARCHIVE_LIB_END', target=lib).split())
            return result

        env['_LIBDEPS_LIBS_WITH_TAGS'] = expand_libdeps_with_extraction_flags

        env['_LIBDEPS_LIBS'] = ('$LINK_WHOLE_ARCHIVE_START '
                                '$LINK_LIBGROUP_START '
                                '$_LIBDEPS_LIBS_WITH_TAGS '
                                '$LINK_LIBGROUP_END '
                                '$LINK_WHOLE_ARCHIVE_END')
        env.Append(
            PROGEMITTER=libdeps_emitter,
            SHLIBEMITTER=libdeps_emitter)
    env.Prepend(_LIBFLAGS='$_LIBDEPS_TAGS $_LIBDEPS $_SYSLIBDEPS ')
    for builder_name in ('Program', 'SharedLibrary', 'LoadableModule'):
        try:
            update_scanner(env['BUILDERS'][builder_name])
        except KeyError:
            pass

def setup_conftests(conf):
    def FindSysLibDep(context, name, libs, **kwargs):
        var = "LIBDEPS_" + name.upper() + "_SYSLIBDEP"
        kwargs['autoadd'] = False
        for lib in libs:
            result = context.sconf.CheckLib(lib, **kwargs)
            context.did_show_result = 1
            if result:
                context.env[var] = lib
                return context.Result(result)
        context.env[var] = __missing_syslib(name)
        return context.Result(result)
    conf.AddTest('FindSysLibDep', FindSysLibDep)
