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
import textwrap

import SCons.Errors
import SCons.Scanner
import SCons.Util


class Constants:
    Libdeps = "LIBDEPS"
    LibdepsPrivate = "LIBDEPS_PRIVATE"
    LibdepsInterface ="LIBDEPS_INTERFACE"
    LibdepsDependents = "LIBDEPS_DEPENDENTS"
    ProgdepsDependents = "PROGDEPS_DEPENDENTS"
    SysLibdeps = "SYSLIBDEPS"
    LibdepsCached = "LIBDEPS_cached"
    SysLibdepsCached = "SYSLIBDEPS_cached"
    MissingLibdep = "MISSING_LIBDEP_"
    LibdepsTags = "LIBDEPS_TAGS"
    LibdepsTagExpansion = "LIBDEPS_TAG_EXPANSIONS"

class dependency(object):
    Public, Private, Interface = list(range(3))

    def __init__(self, value, deptype):
        self.target_node = value
        self.dependency_type = deptype

    def __str__(self):
        return str(self.target_node)

class LibdepLinter(object):
    """
    This class stores the rules for linting the libdeps. Using a decorator,
    new rules can easily be added to the class, and will be called when
    linting occurs. Each rule is run on each libdep.

    When a rule is broken, a LibdepLinterError exception will be raised.
    Optionally the class can be configured to print the error message and
    keep going with the build.

    Each rule should provide a method to skip that rule on a given node,
    by supplying the correct flag in the LIBDEPS_TAG environment var for
    that node.

    """

    skip_linting = False
    print_linter_errors = False

    linting_time = 0
    linting_infractions = 0
    linting_rules_run = 0
    registered_linting_time = False

    @staticmethod
    def _make_linter_decorator():
        """
        This is used for gathering the functions
        by decorator that will be used for linting a given libdep.
        """

        funcs = {}
        def linter_rule_func(func):
            funcs[func.__name__] = func
            return func

        linter_rule_func.all = funcs
        return linter_rule_func
    linter_rule = _make_linter_decorator.__func__()

    def __init__(self, env, target):
        self.env = env
        self.target = target
        self.unique_libs = set()

        # If we are in print mode, we will record some linting metrics,
        # and print the results at the end of the build.
        if self.__class__.print_linter_errors and not self.__class__.registered_linting_time:
            import atexit
            def print_linting_time():
                print(f"Spent {self.__class__.linting_time} seconds linting libdeps.")
                print(f"Found {self.__class__.linting_infractions} issues out of {self.__class__.linting_rules_run} libdeps rules checked.")
            atexit.register(print_linting_time)
            self.__class__.registered_linting_time = True

    def lint_libdeps(self, libdeps):
        """
        Lint the given list of libdeps for all
        rules.
        """

        # Build performance optimization if you
        # are sure your build is clean.
        if self.__class__.skip_linting:
            return

        # Record time spent linting if we are in print mode.
        if self.__class__.print_linter_errors:
            from timeit import default_timer as timer
            start = timer()

        linter_rules = [
            getattr(self, linter_rule)
            for linter_rule in self.linter_rule.all
        ]

        for libdep in libdeps:
            for linter_rule in linter_rules:
                linter_rule(libdep)

        if self.__class__.print_linter_errors:
            self.__class__.linting_time += timer() - start
            self.__class__.linting_rules_run += (len(linter_rules)*len(libdeps))

    def _raise_libdep_lint_exception(self, message):
        """
        Raises the LibdepLinterError exception or if configure
        to do so, just prints the error.
        """
        prefix = "LibdepLinter: \n\t"
        message = prefix + message.replace('\n', '\n\t') + '\n'
        if self.__class__.print_linter_errors:
            self.__class__.linting_infractions += 1
            print(message)
        else:
            raise LibdepLinterError(message)

    def _check_for_lint_tags(self, lint_tag, env=None):
        """
        Used to get the lint tag from the environment,
        and if printing instead of raising exceptions,
        will ignore the tags.
        """

        # ignore LIBDEP_TAGS if printing was selected
        if self.__class__.print_linter_errors:
            return False

        target_env = env if env else self.env

        if lint_tag in target_env.get(Constants.LibdepsTags, []):
            return True

    def _get_deps_dependents(self, env=None):
        """ util function to get all types of DEPS_DEPENDENTS"""
        target_env = env if env else self.env
        deps_dependents = target_env.get(Constants.LibdepsDependents, [])
        deps_dependents += target_env.get(Constants.ProgdepsDependents, [])
        return deps_dependents

    @linter_rule
    def linter_rule_no_dups(self, libdep):
        """
        LIBDEP RULE:
            A given node shall not link the same LIBDEP across public, private
            or interface dependency types because it is ambiguous and unnecessary.
        """
        if self._check_for_lint_tags('lint-allow-dup-libdeps'):
            return

        if str(libdep) in self.unique_libs:
            target_type = self.target[0].builder.get_name(self.env)
            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(
                f"{target_type} '{self.target[0]}' links '{lib}' multiple times."
            )

        self.unique_libs.add(str(libdep))

    @linter_rule
    def linter_rule_programs_link_private(self, libdep):
        """
        LIBDEP RULE:
            All Programs shall only have public dependency's
            because a Program will never be a dependency of another Program
            or Library, and LIBDEPS transitiveness does not apply. Public
            transitiveness has no meaning in this case and is used just as default.
        """
        if self._check_for_lint_tags('lint-allow-program-links-private'):
            return

        if (self.target[0].builder.get_name(self.env) == "Program"
            and libdep.dependency_type != dependency.Public):

            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(
                textwrap.dedent(f"""\
                    Program '{self.target[0]}' links non-public library '{lib}
                    A 'Program' can only have {Constants.Libdeps} libs,
                    not {Constants.LibdepsPrivate} or {Constants.LibdepsInterface}."""
                ))

    @linter_rule
    def linter_rule_no_bidirectional_deps(self, libdep):
        """
        LIBDEP RULE:
            And Library which issues reverse dependencies, shall not be directly
            linked to by another node, to prevent forward and reverse linkages existing
            at the same node. Instead the content of the library that needs to issue reverse
            dependency needs to be separated from content that needs direct linkage into two
            separate libraries, which can be linked correctly respectively.
        """

        if not libdep.target_node.env:
            return
        elif self._check_for_lint_tags('lint-allow-bidirectional-edges', libdep.target_node.env):
            return
        elif len(self._get_deps_dependents(libdep.target_node.env)) > 0:

                target_type = self.target[0].builder.get_name(self.env)
                lib = os.path.basename(str(libdep))
                self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                    {target_type} '{self.target[0]}' links directly to a reverse dependency node '{lib}'
                    No node can link directly to a node that has {Constants.LibdepsDependents} or {Constants.ProgdepsDependents}."""
                ))

    @linter_rule
    def linter_rule_nonprivate_on_deps_dependents(self, libdep):
        """
        LIBDEP RULE:
            A Library that issues reverse dependencies, shall not link libraries
            with any kind of transitiveness, and will only link libraries privately.
            This is because functionality that requires reverse dependencies should
            not be transitive.
        """
        if self._check_for_lint_tags('lint-allow-nonprivate-on-deps-dependents'):
            return

        if (libdep.dependency_type != dependency.Private
            and len(self._get_deps_dependents()) > 0):

            target_type = self.target[0].builder.get_name(self.env)
            lib = os.path.basename(str(libdep))
            self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                {target_type} '{self.target[0]}' links non-private libdep '{lib}' and has a reverse dependency.
                A {target_type} can only have {Constants.LibdepsPrivate} depends if it has {Constants.LibdepsDependents} or {Constants.ProgdepsDependents}."""
            ))

    @linter_rule
    def linter_rule_libdeps_must_be_list(self, libdep):
        """
        LIBDEP RULE:
            LIBDEPS, LIBDEPS_PRIVATE, and LIBDEPS_INTERFACE must be set as lists in the
            environment.
        """
        if self._check_for_lint_tags('lint-allow-nonlist-libdeps'):
            return

        libdeps_vars = list(dep_type_to_env_var.values()) + [
            Constants.LibdepsDependents,
            Constants.ProgdepsDependents]

        for dep_type_val in libdeps_vars:

            libdeps_list = self.env.get(dep_type_val, [])
            if not SCons.Util.is_List(libdeps_list):

                target_type = self.target[0].builder.get_name(self.env)
                self._raise_libdep_lint_exception(textwrap.dedent(f"""\
                    Found non-list type '{libdeps_list}' while evaluating {dep_type_val} for {target_type} '{self.target[0]}'
                    {dep_type_val} must be setup as a list."""
                ))

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

dep_type_to_env_var = {
    dependency.Public: Constants.Libdeps,
    dependency.Private: Constants.LibdepsPrivate,
    dependency.Interface: Constants.LibdepsInterface,
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

class LibdepLinterError(SCons.Errors.UserError):
    """Exception representing a discongruent usages of libdeps"""


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

    cache = getattr(node.attributes, Constants.LibdepsCached, None)
    if cache is not None:
        return cache

    tsorted = []
    marked = set()
    walking = set()

    for child in __get_sorted_direct_libdeps(node):
        if child.dependency_type != dependency.Interface:
            __libdeps_visit(child, marked, tsorted, walking)

    tsorted.reverse()
    setattr(node.attributes, Constants.LibdepsCached, tsorted)

    return tsorted


def __get_syslibdeps(node):
    """ Given a SCons Node, return its system library dependencies.

    These are the depencencies listed with SYSLIBDEPS, and are linked using -l.
    """
    result = getattr(node.attributes, Constants.SysLibdepsCached, None)
    if result is not None:
        return result

    result = node.get_env().Flatten(node.get_env().get(Constants.SysLibdeps, []))
    for lib in __get_libdeps(node):
        for syslib in lib.get_env().get(Constants.SysLibdeps, []):
            if not syslib:
                continue

            if type(syslib) is str and syslib.startswith(Constants.MissingLibdep):
                print(
                    "Target '{}' depends on the availability of a "
                    "system provided library for '{}', "
                    "but no suitable library was found during configuration.".format(str(node), syslib[len(Constants.MissingLibdep) :])
                )
                node.get_env().Exit(1)

            result.append(syslib)

    setattr(node.attributes, Constants.SysLibdepsCached, result)
    return result


def __missing_syslib(name):
    return Constants.MissingLibdep + name


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


def __get_node_with_ixes(env, node, node_builder_type):
    """
    Gets the node passed in node with the correct ixes applied
    for the given builder type.
    """

    if not node:
        return node

    node_builder = env["BUILDERS"][node_builder_type]
    node_factory = node_builder.target_factory or env.File

    # Cache the ixes in a function scope global so we don't need
    # to run scons performance intensive 'subst' each time
    cache_key = (id(env), node_builder_type)
    try:
        prefix, suffix = __get_node_with_ixes.node_type_ixes[cache_key]
    except KeyError:
        prefix = node_builder.get_prefix(env)
        suffix = node_builder.get_suffix(env)
        __get_node_with_ixes.node_type_ixes[cache_key] = (prefix, suffix)

    node_with_ixes = SCons.Util.adjustixes(node, prefix, suffix)
    return node_factory(node_with_ixes)

__get_node_with_ixes.node_type_ixes = dict()

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

        # Get all the libdeps from the env so we can
        # can append them to the current target_node.
        libdeps = []
        for dep_type in sorted(dependency_map.keys()):

            # Libraries may not be stored as a list in the env,
            # so we must convert single library strings to a list.
            libs = env.get(dep_type_to_env_var[dep_type])
            if not SCons.Util.is_List(libs):
                libs = [libs]

            for lib in libs:
                if not lib:
                    continue
                lib_with_ixes = __get_node_with_ixes(env, lib, dependency_builder)
                libdeps.append(dependency(lib_with_ixes, dep_type))

        # Lint the libdeps to make sure they are following the rules.
        # This will skip some or all of the checks depending on the options
        # and LIBDEPS_TAGS used.
        LibdepLinter(env, target).lint_libdeps(libdeps)

        # We ignored the dependency_map until now because we needed to use
        # original dependency value for linting. Now go back through and
        # use the map to convert to the desired dependencies, for example
        # all Public in the static linking case.
        for libdep in libdeps:
            libdep.dependency_type = dependency_map[libdep.dependency_type]

        for t in target:
            # target[0] must be a Node and not a string, or else libdeps will fail to
            # work properly.
            __append_direct_libdeps(t, libdeps)

        for dependent in env.get(Constants.LibdepsDependents, []):
            if dependent is None:
                continue

            visibility = dependency.Private
            if isinstance(dependent, tuple):
                visibility = dependent[1]
                dependent = dependent[0]

            dependentNode = __get_node_with_ixes(
                env, dependent, dependency_builder
            )
            __append_direct_libdeps(
                dependentNode, [dependency(target[0], dependency_map[visibility])]
            )

        if not ignore_progdeps:
            for dependent in env.get(Constants.ProgdepsDependents, []):
                if dependent is None:
                    continue

                visibility = dependency.Public
                if isinstance(dependent, tuple):
                    # TODO: Error here? Non-public PROGDEPS_DEPENDENTS probably are meaningless
                    visibility = dependent[1]
                    dependent = dependent[0]

                dependentNode = __get_node_with_ixes(
                    env, dependent, "Program"
                )
                __append_direct_libdeps(
                    dependentNode, [dependency(target[0], dependency_map[visibility])]
                )

        return target, source

    return libdeps_emitter


def expand_libdeps_tags(source, target, env, for_signature):
    results = []
    for expansion in env.get(Constants.LibdepsTagExpansion, []):
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
            lib_tags = lib.get_env().get(Constants.LibdepsTags, [])
        else:
            lib_target = env.subst("$TARGET", target=lib)
            lib_tags = env.File(lib).get_env().get(Constants.LibdepsTags, [])

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


def setup_environment(env, emitting_shared=False, linting='on'):
    """Set up the given build environment to do LIBDEPS tracking."""

    LibdepLinter.skip_linting = linting == 'off'
    LibdepLinter.print_linter_errors = linting == 'print'

    try:
        env["_LIBDEPS"]
    except KeyError:
        env["_LIBDEPS"] = "$_LIBDEPS_LIBS"

    env["_LIBDEPS_TAGS"] = expand_libdeps_tags
    env["_LIBDEPS_GET_LIBS"] = get_libdeps
    env["_LIBDEPS_OBJS"] = get_libdeps_objs
    env["_SYSLIBDEPS"] = get_syslibdeps

    env[Constants.Libdeps] = SCons.Util.CLVar()
    env[Constants.SysLibdeps] = SCons.Util.CLVar()

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
