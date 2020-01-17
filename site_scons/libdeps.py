"""Extension to SCons providing advanced static library dependency tracking.

These modifications to a build environment, which can be attached to
StaticLibrary and Program builders via a call to setup_environment(env),
cause the build system to track library dependencies through static libraries,
and to add them to the link command executed when building programs.

For example, consider a program 'try' that depends on a lib 'tc', which in
turn uses a symbol from a lib 'tb' which in turn uses a library from 'ta'.

Without this package, the Program declaration for "try" looks like this:

Program('try', ['try.c', 'path/to/${LIBPREFIX}tc${LIBSUFFIX}',
                'path/to/${LIBPREFIX}tb${LIBSUFFIX}',
                'path/to/${LIBPREFIX}ta${LIBSUFFIX}',])

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

libdeps_env_var = "LIBDEPS"
syslibdeps_env_var = "SYSLIBDEPS"
missing_syslibdep = "MISSING_LIBDEP_"


class dependency(object):
    Public, Private, Interface = list(range(3))

    def __init__(self, value, deptype):
        self.target_node = value
        self.dependency_type = deptype

    def __str__(self):
        return str(self.target_node)


dependency_visibility_ignored = {
    dependency.Public: dependency.Public,
    dependency.Private: dependency.Public,
    dependency.Interface: dependency.Public,
}

dependency_visibility_honored = {
    dependency.Public: dependency.Public,
    dependency.Private: dependency.Private,
    dependency.Interface: dependency.Interface,
}


class DependencyCycleError(SCons.Errors.UserError):
    """Exception representing a cycle discovered in library dependencies."""

    def __init__(self, first_node):
        super(DependencyCycleError, self).__init__()
        self.cycle_nodes = [first_node]

    def __str__(self):
        return "Library dependency cycle detected: " + " => ".join(
            str(n) for n in self.cycle_nodes
        )


def __get_sorted_direct_libdeps(node):
    direct_sorted = getattr(node.attributes, "libdeps_direct_sorted", False)
    if not direct_sorted:
        direct = getattr(node.attributes, "libdeps_direct", [])
        direct_sorted = sorted(direct, key=lambda t: str(t.target_node))
        setattr(node.attributes, "libdeps_direct_sorted", direct_sorted)
    return direct_sorted


def __libdeps_visit(n, marked, tsorted, walking):
    if n.target_node in marked:
        return

    if n.target_node in walking:
        raise DependencyCycleError(n.target_node)

    walking.add(n.target_node)

    try:
        for child in __get_sorted_direct_libdeps(n.target_node):
            if child.dependency_type != dependency.Private:
                __libdeps_visit(child, marked, tsorted, walking=walking)

        marked.add(n.target_node)
        tsorted.append(n.target_node)

    except DependencyCycleError as e:
        if len(e.cycle_nodes) == 1 or e.cycle_nodes[0] != e.cycle_nodes[-1]:
            e.cycle_nodes.insert(0, n.target_node)
        raise


def __get_libdeps(node):
    """Given a SCons Node, return its library dependencies, topologically sorted.

    Computes the dependencies if they're not already cached.
    """

    cached_var_name = libdeps_env_var + "_cached"

    cache = getattr(node.attributes, cached_var_name, None)
    if cache is not None:
        return cache

    tsorted = []
    marked = set()
    walking = set()

    for child in __get_sorted_direct_libdeps(node):
        if child.dependency_type != dependency.Interface:
            __libdeps_visit(child, marked, tsorted, walking)

    tsorted.reverse()
    setattr(node.attributes, cached_var_name, tsorted)

    return tsorted


def __get_syslibdeps(node):
    """ Given a SCons Node, return its system library dependencies.

    These are the depencencies listed with SYSLIBDEPS, and are linked using -l.
    """
    cached_var_name = syslibdeps_env_var + "_cached"
    result = getattr(node.attributes, cached_var_name, None)
    if result is not None:
        return result

    result = node.get_env().Flatten(node.get_env().get(syslibdeps_env_var, []))
    for lib in __get_libdeps(node):
        for syslib in lib.get_env().get(syslibdeps_env_var, []):
            if not syslib:
                continue

            if type(syslib) is str and syslib.startswith(missing_syslibdep):
                print(
                    "Target '{}' depends on the availability of a "
                    "system provided library for '{}', "
                    "but no suitable library was found during configuration.".format(str(node), syslib[len(missing_syslibdep) :])
                )
                node.get_env().Exit(1)

            result.append(syslib)

    setattr(node.attributes, cached_var_name, result)
    return result


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

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner, path_function=path_function
    )


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
    lib_link_prefix = env.subst("$LIBLINKPREFIX")
    lib_link_suffix = env.subst("$LIBLINKSUFFIX")
    result = []
    for d in deps:
        # Elements of syslibdeps are either strings (str or unicode), or they're File objects.
        # If they're File objects, they can be passed straight through.  If they're strings,
        # they're believed to represent library short names, that should be prefixed with -l
        # or the compiler-specific equivalent.  I.e., 'm' becomes '-lm', but 'File("m.a") is passed
        # through whole cloth.
        if type(d) is str:
            result.append("%s%s%s" % (lib_link_prefix, d, lib_link_suffix))
        else:
            result.append(d)
    return result


def __append_direct_libdeps(node, prereq_nodes):
    # We do not bother to decorate nodes that are not actual Objects
    if type(node) == str:
        return
    if getattr(node.attributes, "libdeps_direct", None) is None:
        node.attributes.libdeps_direct = []
    node.attributes.libdeps_direct.extend(prereq_nodes)


def make_libdeps_emitter(
    dependency_builder,
    dependency_map=dependency_visibility_ignored,
    ignore_progdeps=False,
):
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

        lib_builder = env["BUILDERS"][dependency_builder]
        lib_node_factory = lib_builder.target_factory or env.File

        prog_builder = env["BUILDERS"]["Program"]
        prog_node_factory = prog_builder.target_factory or env.File

        prereqs = [
            dependency(l, dependency_map[dependency.Public])
            for l in env.get(libdeps_env_var, [])
            if l
        ]
        prereqs.extend(
            dependency(l, dependency_map[dependency.Interface])
            for l in env.get(libdeps_env_var + "_INTERFACE", [])
            if l
        )
        prereqs.extend(
            dependency(l, dependency_map[dependency.Private])
            for l in env.get(libdeps_env_var + "_PRIVATE", [])
            if l
        )

        lib_builder_prefix = lib_builder.get_prefix(env)
        lib_builder_suffix = lib_builder.get_suffix(env)

        for prereq in prereqs:
            prereqWithIxes = SCons.Util.adjustixes(
                prereq.target_node, lib_builder_prefix, lib_builder_suffix
            )
            prereq.target_node = lib_node_factory(prereqWithIxes)

        for t in target:
            # target[0] must be a Node and not a string, or else libdeps will fail to
            # work properly.
            __append_direct_libdeps(t, prereqs)

        for dependent in env.get("LIBDEPS_DEPENDENTS", []):
            if dependent is None:
                continue

            visibility = dependency.Private
            if isinstance(dependent, tuple):
                visibility = dependent[1]
                dependent = dependent[0]

            dependentWithIxes = SCons.Util.adjustixes(
                dependent, lib_builder_prefix, lib_builder_suffix
            )
            dependentNode = lib_node_factory(dependentWithIxes)
            __append_direct_libdeps(
                dependentNode, [dependency(target[0], dependency_map[visibility])]
            )

        prog_builder_prefix = prog_builder.get_prefix(env)
        prog_builder_suffix = prog_builder.get_suffix(env)

        if not ignore_progdeps:
            for dependent in env.get("PROGDEPS_DEPENDENTS", []):
                if dependent is None:
                    continue

                visibility = dependency.Public
                if isinstance(dependent, tuple):
                    # TODO: Error here? Non-public PROGDEPS_DEPENDENTS probably are meaningless
                    visibility = dependent[1]
                    dependent = dependent[0]

                dependentWithIxes = SCons.Util.adjustixes(
                    dependent, prog_builder_prefix, prog_builder_suffix
                )
                dependentNode = prog_node_factory(dependentWithIxes)
                __append_direct_libdeps(
                    dependentNode, [dependency(target[0], dependency_map[visibility])]
                )

        return target, source

    return libdeps_emitter


def expand_libdeps_tags(source, target, env, for_signature):
    results = []
    for expansion in env.get("LIBDEPS_TAG_EXPANSIONS", []):
        results.append(expansion(source, target, env, for_signature))
    return results


def expand_libdeps_with_extraction_flags(source, target, env, for_signature):
    result = []
    libs = get_libdeps(source, target, env, for_signature)
    whole_archive_start = env.subst("$LINK_WHOLE_ARCHIVE_LIB_START")
    whole_archive_end = env.subst("$LINK_WHOLE_ARCHIVE_LIB_END")
    whole_archive_separator = env.get("LINK_WHOLE_ARCHIVE_SEP", " ")
    for lib in libs:
        if isinstance(lib, (str, SCons.Node.FS.File, SCons.Node.FS.Entry)):
            lib_target = str(lib)
            lib_tags = lib.get_env().get("LIBDEPS_TAGS", [])
        else:
            lib_target = env.subst("$TARGET", target=lib)
            lib_tags = env.File(lib).get_env().get("LIBDEPS_TAGS", [])

        if "init-no-global-side-effects" in lib_tags:
            result.append(lib_target)
        else:
            whole_archive_flag = "{}{}{}".format(
                whole_archive_start, whole_archive_separator, lib_target
            )
            if whole_archive_end:
                whole_archive_flag += "{}{}".format(
                    whole_archive_separator, whole_archive_end
                )

            result.extend(whole_archive_flag.split())

    return result


def setup_environment(env, emitting_shared=False):
    """Set up the given build environment to do LIBDEPS tracking."""

    try:
        env["_LIBDEPS"]
    except KeyError:
        env["_LIBDEPS"] = "$_LIBDEPS_LIBS"

    env["_LIBDEPS_TAGS"] = expand_libdeps_tags
    env["_LIBDEPS_GET_LIBS"] = get_libdeps
    env["_LIBDEPS_OBJS"] = get_libdeps_objs
    env["_SYSLIBDEPS"] = get_syslibdeps

    env[libdeps_env_var] = SCons.Util.CLVar()
    env[syslibdeps_env_var] = SCons.Util.CLVar()

    # We need a way for environments to alter just which libdeps
    # emitter they want, without altering the overall program or
    # library emitter which may have important effects. The
    # subsitution rules for emitters are a little strange, so build
    # ourselves a little trampoline to use below so we don't have to
    # deal with it.
    def make_indirect_emitter(variable):
        def indirect_emitter(target, source, env):
            return env[variable](target, source, env)

        return indirect_emitter

    env.Append(
        LIBDEPS_LIBEMITTER=make_libdeps_emitter("StaticLibrary"),
        LIBEMITTER=make_indirect_emitter("LIBDEPS_LIBEMITTER"),
        LIBDEPS_SHAREMITTER=make_libdeps_emitter("SharedArchive", ignore_progdeps=True),
        SHAREMITTER=make_indirect_emitter("LIBDEPS_SHAREMITTER"),
        LIBDEPS_SHLIBEMITTER=make_libdeps_emitter(
            "SharedLibrary", dependency_visibility_honored
        ),
        SHLIBEMITTER=make_indirect_emitter("LIBDEPS_SHLIBEMITTER"),
        LIBDEPS_PROGEMITTER=make_libdeps_emitter(
            "SharedLibrary" if emitting_shared else "StaticLibrary"
        ),
        PROGEMITTER=make_indirect_emitter("LIBDEPS_PROGEMITTER"),
    )

    env["_LIBDEPS_LIBS_WITH_TAGS"] = expand_libdeps_with_extraction_flags

    env["_LIBDEPS_LIBS"] = (
        "$LINK_WHOLE_ARCHIVE_START "
        "$LINK_LIBGROUP_START "
        "$_LIBDEPS_LIBS_WITH_TAGS "
        "$LINK_LIBGROUP_END "
        "$LINK_WHOLE_ARCHIVE_END"
    )

    env.Prepend(_LIBFLAGS="$_LIBDEPS_TAGS $_LIBDEPS $_SYSLIBDEPS ")
    for builder_name in ("Program", "SharedLibrary", "LoadableModule", "SharedArchive"):
        try:
            update_scanner(env["BUILDERS"][builder_name])
        except KeyError:
            pass


def setup_conftests(conf):
    def FindSysLibDep(context, name, libs, **kwargs):
        var = "LIBDEPS_" + name.upper() + "_SYSLIBDEP"
        kwargs["autoadd"] = False
        for lib in libs:
            result = context.sconf.CheckLib(lib, **kwargs)
            context.did_show_result = 1
            if result:
                context.env[var] = lib
                return context.Result(result)
        context.env[var] = __missing_syslib(name)
        return context.Result(result)

    conf.AddTest("FindSysLibDep", FindSysLibDep)
