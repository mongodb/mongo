# Copyright 2020 MongoDB Inc.
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
#
"""Generate build.ninja files from SCons aliases."""

import sys
import os
import importlib
import io
import shutil
import shlex
import tempfile
import textwrap

from collections import OrderedDict
from glob import glob
from os.path import join as joinpath
from os.path import splitext

import SCons
from SCons.Action import _string_from_cmd_list, get_default_ENV
from SCons.Util import is_List, flatten_sequence
from SCons.Script import COMMAND_LINE_TARGETS

NINJA_STATE = None
NINJA_SYNTAX = "NINJA_SYNTAX"
NINJA_RULES = "__NINJA_CUSTOM_RULES"
NINJA_POOLS = "__NINJA_CUSTOM_POOLS"
NINJA_CUSTOM_HANDLERS = "__NINJA_CUSTOM_HANDLERS"
NINJA_BUILD = "NINJA_BUILD"
NINJA_WHEREIS_MEMO = {}
NINJA_STAT_MEMO = {}

__NINJA_RULE_MAPPING = {}

# These are the types that get_command can do something with
COMMAND_TYPES = (
    SCons.Action.CommandAction,
    SCons.Action.CommandGeneratorAction,
)

ninja_compdb_adjust = """\
import json
import sys

compdb = {}
with open(sys.argv[1]) as f:
    compdb = json.load(f)

for command in compdb:
    if command['output'].endswith('.compdb'):
        command['output'] = command['output'][:-(len('.compdb'))]
    else:
        print(f"compdb entry does not contain '.compdb': {command['output']}")

with open(sys.argv[1], 'w') as f:
    json.dump(compdb, f, indent=2)
"""


def _install_action_function(_env, node):
    """Install files using the install or copy commands"""
    return {
        "outputs": get_outputs(node),
        "rule": "INSTALL",
        "inputs": [get_path(src_file(s)) for s in node.sources],
        "implicit": get_dependencies(node),
        "variables": {"precious": node.precious},
    }


def _mkdir_action_function(env, node):
    return {
        "outputs": get_outputs(node),
        "rule": "CMD",
        # implicit explicitly omitted, we translate these so they can be
        # used by anything that depends on these but commonly this is
        # hit with a node that will depend on all of the fake
        # srcnode's that SCons will never give us a rule for leading
        # to an invalid ninja file.
        "variables": {
            # On Windows mkdir "-p" is always on
            "cmd":
                "mkdir {args}".format(
                    args=' '.join(get_outputs(node)) + " & exit /b 0"
                    if env["PLATFORM"] == "win32" else "-p " + ' '.join(get_outputs(node)), ),
            "variables": {"precious": node.precious},
        },
    }


def _lib_symlink_action_function(_env, node):
    """Create shared object symlinks if any need to be created"""
    symlinks = getattr(getattr(node, "attributes", None), "shliblinks", None)

    if not symlinks or symlinks is None:
        return None

    outputs = [link.get_dir().rel_path(linktgt) for link, linktgt in symlinks]
    inputs = [link.get_path() for link, _ in symlinks]

    return {
        "outputs": outputs,
        "inputs": inputs,
        "rule": "SYMLINK",
        "implicit": get_dependencies(node),
        "variables": {"precious": node.precious},
    }


def is_valid_dependent_node(node):
    """
    Return True if node is not an alias or is an alias that has children

    This prevents us from making phony targets that depend on other
    phony targets that will never have an associated ninja build
    target.

    We also have to specify that it's an alias when doing the builder
    check because some nodes (like src files) won't have builders but
    are valid implicit dependencies.
    """
    if isinstance(node, SCons.Node.Alias.Alias):
        return node.children()

    if not node.env:
        return True

    return not node.env.get("NINJA_SKIP")


def alias_to_ninja_build(node):
    """Convert an Alias node into a Ninja phony target"""
    return {
        "outputs": get_outputs(node),
        "rule": "phony",
        "implicit": [get_path(src_file(n)) for n in node.children() if is_valid_dependent_node(n)],
    }


def get_order_only(node):
    """Return a list of order only dependencies for node."""
    if node.prerequisites is None:
        return []
    return [
        get_path(src_file(prereq)) for prereq in node.prerequisites
        if is_valid_dependent_node(prereq)
    ]


def get_dependencies(node, skip_sources=False):
    """Return a list of dependencies for node."""
    if skip_sources:
        return [
            get_path(src_file(child)) for child in node.children()
            if child not in node.sources and is_valid_dependent_node(child)
        ]
    return [
        get_path(src_file(child)) for child in node.children() if is_valid_dependent_node(child)
    ]


def get_inputs(node, skip_unknown_types=False):
    """
    Collect the Ninja inputs for node.

    If the given node has inputs which can not be converted into something
    Ninja can process, this will throw an exception. Optionally, those nodes
    that are not processable can be skipped as inputs with the
    skip_unknown_types keyword arg.
    """
    executor = node.get_executor()
    if executor is not None:
        inputs = executor.get_all_sources()
    else:
        inputs = node.sources

    # Some Nodes (e.g. Python.Value Nodes) won't have files associated. We allow these to be
    # optionally skipped to enable the case where we will re-invoke SCons for things
    # like TEMPLATE. Otherwise, we have no direct way to express the behavior for such
    # Nodes in Ninja, so we raise a hard error
    ninja_nodes = []
    for input_node in inputs:
        if isinstance(input_node, (SCons.Node.FS.Base, SCons.Node.Alias.Alias)):
            ninja_nodes.append(input_node)
        else:
            if skip_unknown_types:
                continue
            raise Exception(
                "Can't process {} node '{}' as an input for '{}'".format(
                    type(input_node),
                    str(input_node),
                    str(node),
                ), )

    # convert node items into raw paths/aliases for ninja
    return [get_path(src_file(o)) for o in ninja_nodes]


def get_outputs(node):
    """Collect the Ninja outputs for node."""
    executor = node.get_executor()
    if executor is not None:
        outputs = executor.get_all_targets()
    else:
        if hasattr(node, "target_peers"):
            outputs = node.target_peers
        else:
            outputs = [node]

    outputs = [get_path(o) for o in outputs]

    return outputs


def generate_depfile(env, node, dependencies):
    """
    Ninja tool function for writing a depfile. The depfile should include
    the node path followed by all the dependent files in a makefile format.

    dependencies arg can be a list or a subst generator which returns a list.
    """

    depfile = os.path.join(get_path(env['NINJA_BUILDDIR']), str(node) + '.depfile')

    # subst_list will take in either a raw list or a subst callable which generates
    # a list, and return a list of CmdStringHolders which can be converted into raw strings.
    # If a raw list was passed in, then scons_list will make a list of lists from the original
    # values and even subst items in the list if they are substitutable. Flatten will flatten
    # the list in that case, to ensure for either input we have a list of CmdStringHolders.
    deps_list = env.Flatten(env.subst_list(dependencies))

    # Now that we have the deps in a list as CmdStringHolders, we can convert them into raw strings
    # and make sure to escape the strings to handle spaces in paths. We also will sort the result
    # keep the order of the list consistent.
    escaped_depends = sorted([dep.escape(env.get("ESCAPE", lambda x: x)) for dep in deps_list])
    depfile_contents = str(node) + ": " + ' '.join(escaped_depends)

    need_rewrite = False
    try:
        with open(depfile, 'r') as f:
            need_rewrite = (f.read() != depfile_contents)
    except FileNotFoundError:
        need_rewrite = True

    if need_rewrite:
        os.makedirs(os.path.dirname(depfile) or '.', exist_ok=True)
        with open(depfile, 'w') as f:
            f.write(depfile_contents)


class SConsToNinjaTranslator:
    """Translates SCons Actions into Ninja build objects."""

    def __init__(self, env):
        self.env = env
        self.func_handlers = {
            # Skip conftest builders
            "_createSource": ninja_noop,
            # SCons has a custom FunctionAction that just makes sure the
            # target isn't static. We let the commands that ninja runs do
            # this check for us.
            "SharedFlagChecker": ninja_noop,
            # The install builder is implemented as a function action.
            "installFunc": _install_action_function,
            "MkdirFunc": _mkdir_action_function,
            "LibSymlinksActionFunction": _lib_symlink_action_function,
        }

        self.loaded_custom = False

    def action_to_ninja_build(self, node, action=None):
        """Generate build arguments dictionary for node."""
        if not self.loaded_custom:
            self.func_handlers.update(self.env[NINJA_CUSTOM_HANDLERS])
            self.loaded_custom = True

        if node.builder is None:
            return None

        if action is None:
            action = node.builder.action

        if node.env and node.env.get("NINJA_SKIP"):
            return None

        build = {}
        env = node.env if node.env else self.env

        # Ideally this should never happen, and we do try to filter
        # Ninja builders out of being sources of ninja builders but I
        # can't fix every DAG problem so we just skip ninja_builders
        # if we find one
        if node.builder == self.env["BUILDERS"]["Ninja"]:
            build = None
        elif isinstance(action, SCons.Action.FunctionAction):
            build = self.handle_func_action(node, action)
        elif isinstance(action, SCons.Action.LazyAction):
            # pylint: disable=protected-access
            action = action._generate_cache(env)
            build = self.action_to_ninja_build(node, action=action)
        elif isinstance(action, SCons.Action.ListAction):
            build = self.handle_list_action(node, action)
        elif isinstance(action, COMMAND_TYPES):
            build = get_command(env, node, action)
        else:
            raise Exception("Got an unbuildable ListAction for: {}".format(str(node)))

        if build is not None:
            build["order_only"] = get_order_only(node)

        if 'conftest' not in str(node):
            node_callback = getattr(node.attributes, "ninja_build_callback", None)
            if callable(node_callback):
                node_callback(env, node, build)

        if build is not None and node.precious:
            if not build.get('variables'):
                build['variables'] = {}
            build['variables']['precious'] = node.precious

        return build

    def handle_func_action(self, node, action):
        """Determine how to handle the function action."""
        name = action.function_name()
        # This is the name given by the Subst/Textfile builders. So return the
        # node to indicate that SCons is required. We skip sources here because
        # dependencies don't really matter when we're going to shove these to
        # the bottom of ninja's DAG anyway and Textfile builders can have text
        # content as their source which doesn't work as an implicit dep in
        # ninja. We suppress errors on input Nodes types that we cannot handle
        # since we expect that the re-invocation of SCons will handle dependency
        # tracking for those Nodes and their dependents.
        if name == "_action":
            return {
                "rule": "TEMPLATE",
                "outputs": get_outputs(node),
                "inputs": get_inputs(node, skip_unknown_types=True),
                "implicit": get_dependencies(node, skip_sources=True),
            }

        handler = self.func_handlers.get(name, None)
        if handler is not None:
            return handler(node.env if node.env else self.env, node)

        raise Exception("Found unhandled function action {}, "
                        " generating scons command to build\n"
                        "Note: this is less efficient than Ninja,"
                        " you can write your own ninja build generator for"
                        " this function using NinjaRegisterFunctionHandler".format(name))

    def handle_list_action(self, node, action):
        """TODO write this comment"""
        results = [
            self.action_to_ninja_build(node, action=act) for act in action.list if act is not None
        ]
        results = [result for result in results if result is not None and result["outputs"]]
        if not results:
            return None

        # No need to process the results if we only got a single result
        if len(results) == 1:
            return results[0]

        all_outputs = list({output for build in results for output in build["outputs"]})
        dependencies = list({dep for build in results for dep in build["implicit"]})

        if results[0]["rule"] == "CMD":
            cmdline = ""
            for cmd in results:

                # Occasionally a command line will expand to a
                # whitespace only string (i.e. '  '). Which is not a
                # valid command but does not trigger the empty command
                # condition if not cmdstr. So here we strip preceding
                # and proceeding whitespace to make strings like the
                # above become empty strings and so will be skipped.
                cmdstr = cmd["variables"]["cmd"].strip()
                if not cmdstr:
                    continue

                # Skip duplicate commands
                if cmdstr in cmdline:
                    continue

                if cmdline:
                    cmdline += " && "

                cmdline += cmdstr

            # Remove all preceding and proceeding whitespace
            cmdline = cmdline.strip()

            # Make sure we didn't generate an empty cmdline
            if cmdline:

                env = node.env if node.env else self.env
                sources = [get_path(src_file(s)) for s in node.sources]

                ninja_build = {
                    "outputs": all_outputs,
                    "rule": "CMD",
                    "variables": {
                        "cmd": cmdline,
                        "env": get_command_env(env, all_outputs, sources),
                    },
                    "implicit": dependencies,
                }

                if node.env and node.env.get("NINJA_POOL", None) is not None:
                    ninja_build["pool"] = node.env["pool"]

                return ninja_build

        elif results[0]["rule"] == "phony":
            return {
                "outputs": all_outputs,
                "rule": "phony",
                "implicit": dependencies,
            }

        raise Exception("Unhandled list action with rule: " + results[0]["rule"])


class NinjaState:
    """Maintains state of Ninja build system as it's translated from SCons."""

    def __init__(self, env, ninja_syntax):
        self.env = env
        self.writer_class = ninja_syntax.Writer
        self.__generated = False
        self.translator = SConsToNinjaTranslator(env)
        self.generated_suffixes = env.get("NINJA_GENERATED_SOURCE_SUFFIXES", [])

        # List of generated builds that will be written at a later stage
        self.builds = dict()

        # List of targets for which we have generated a build. This
        # allows us to take multiple Alias nodes as sources and to not
        # fail to build if they have overlapping targets.
        self.built = set()

        # SCons sets this variable to a function which knows how to do
        # shell quoting on whatever platform it's run on. Here we use it
        # to make the SCONS_INVOCATION variable properly quoted for things
        # like CCFLAGS
        scons_escape = env.get("ESCAPE", lambda x: x)

        self.variables = {
            # The /b option here will make sure that windows updates the mtime
            # when copying the file. This allows to not need to use restat for windows
            # copy commands.
            "COPY":
                "cmd.exe /c 1>NUL copy /b" if sys.platform == "win32" else "cp",
            "NOOP":
                "cmd.exe /c 1>NUL echo 0" if sys.platform == "win32" else "echo 0 >/dev/null",
            "SCONS_INVOCATION":
                "{} {} __NINJA_NO=1 $out".format(
                    sys.executable,
                    " ".join([
                        ninja_syntax.escape(scons_escape(arg)) for arg in sys.argv
                        if arg not in COMMAND_LINE_TARGETS
                    ]),
                ),
            "SCONS_INVOCATION_W_TARGETS":
                "{} {}".format(
                    sys.executable,
                    " ".join([ninja_syntax.escape(scons_escape(arg)) for arg in sys.argv])),
            # This must be set to a global default per:
            # https://ninja-build.org/manual.html
            #
            # (The deps section)
            "msvc_deps_prefix":
                "Note: including file:",
        }

        self.rules = {
            "CMD": {
                "command": "cmd /c $env$cmd" if sys.platform == "win32" else "$env$cmd",
                "description": "Built $out",
                "pool": "local_pool",
            },
            # We add the deps processing variables to this below. We
            # don't pipe these through cmd.exe on Windows because we
            # use this to generate a compile_commands.json database
            # which can't use the shell command as it's compile
            # command.
            "CC": {
                "command": "$env$CC @$out.rsp",
                "description": "Compiled $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
            },
            "CXX": {
                "command": "$env$CXX @$out.rsp",
                "description": "Compiled $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
            },
            "COMPDB_CC": {
                "command": "$CC @$out.rsp",
                "description": "Compiling $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
            },
            "COMPDB_CXX": {
                "command": "$CXX @$out.rsp",
                "description": "Compiling $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
            },
            "LINK": {
                "command": "$env$LINK @$out.rsp",
                "description": "Linked $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
                "pool": "local_pool",
            },
            "AR": {
                "command": "$env$AR @$out.rsp",
                "description": "Archived $out",
                "rspfile": "$out.rsp",
                "rspfile_content": "$rspc",
                "pool": "local_pool",
            },
            "SYMLINK": {
                "command": (
                    "cmd /c mklink $out $in" if sys.platform == "win32" else "ln -s $in $out"),
                "description": "Symlinked $in -> $out",
            },
            "NOOP": {
                "command": "$NOOP",
                "description": "Checked $out",
                "pool": "local_pool",
            },
            "INSTALL": {
                "command": "$COPY $in $out",
                "description": "Installed $out",
                "pool": "install_pool",
            },
            "TEMPLATE": {
                "command": "$SCONS_INVOCATION $out",
                "description": "Rendered $out",
                "pool": "scons_pool",
                "restat": 1,
            },
            "SCONS": {
                "command": "$SCONS_INVOCATION $out",
                "description": "SCons $out",
                "pool": "scons_pool",
                # restat
                #    if present, causes Ninja to re-stat the command's outputs
                #    after execution of the command. Each output whose
                #    modification time the command did not change will be
                #    treated as though it had never needed to be built. This
                #    may cause the output's reverse dependencies to be removed
                #    from the list of pending build actions.
                #
                # We use restat any time we execute SCons because
                # SCons calls in Ninja typically create multiple
                # targets. But since SCons is doing it's own up to
                # date-ness checks it may only update say one of
                # them. Restat will find out which of the multiple
                # build targets did actually change then only rebuild
                # those targets which depend specifically on that
                # output.
                "restat": 1,
            },
            "REGENERATE": {
                "command": "$SCONS_INVOCATION_W_TARGETS",
                "description": "Regenerated $self",
                "depfile": os.path.join(get_path(env['NINJA_BUILDDIR']), '$out.depfile'),
                "generator": 1,
                # Console pool restricts to 1 job running at a time,
                # it additionally has some special handling about
                # passing stdin, stdout, etc to process in this pool
                # that we need for SCons to behave correctly when
                # regenerating Ninja
                "pool": "console",
                # Again we restat in case Ninja thought the
                # build.ninja should be regenerated but SCons knew
                # better.
                "restat": 1,
            },
        }
        num_jobs = self.env.get('NINJA_MAX_JOBS', self.env.GetOption("num_jobs"))
        self.pools = {
            "local_pool": num_jobs,
            "install_pool": num_jobs / 2,
            "scons_pool": 1,
        }

        for rule in ["CC", "CXX"]:
            if env["PLATFORM"] == "win32":
                self.rules[rule]["deps"] = "msvc"
            else:
                self.rules[rule]["deps"] = "gcc"
                self.rules[rule]["depfile"] = "$out.d"

    def add_build(self, node):
        if not node.has_builder():
            return False

        if isinstance(node, SCons.Node.Alias.Alias):
            build = alias_to_ninja_build(node)
        else:
            build = self.translator.action_to_ninja_build(node)

        # Some things are unbuild-able or need not be built in Ninja
        if build is None:
            return False

        node_string = str(node)
        if node_string in self.builds:
            raise Exception("Node {} added to ninja build state more than once".format(node_string))
        self.builds[node_string] = build
        self.built.update(build["outputs"])
        return True

    def is_generated_source(self, output):
        """Check if output ends with a known generated suffix."""
        _, suffix = splitext(output)
        return suffix in self.generated_suffixes

    def has_generated_sources(self, output):
        """
        Determine if output indicates this is a generated header file.
        """
        for generated in output:
            if self.is_generated_source(generated):
                return True
        return False

    def generate(self, ninja_file):
        """
        Generate the build.ninja.

        This should only be called once for the lifetime of this object.
        """
        if self.__generated:
            return

        self.rules.update(self.env.get(NINJA_RULES, {}))
        self.pools.update(self.env.get(NINJA_POOLS, {}))

        content = io.StringIO()
        ninja = self.writer_class(content, width=100)

        ninja.comment("Generated by scons. DO NOT EDIT.")

        # This version is needed because it is easy to get from pip and it support compile_commands.json
        ninja.variable("ninja_required_version", "1.10")
        ninja.variable("builddir", get_path(self.env['NINJA_BUILDDIR']))

        for pool_name, size in self.pools.items():
            ninja.pool(pool_name, min(self.env.get('NINJA_MAX_JOBS', size), size))

        for var, val in self.variables.items():
            ninja.variable(var, val)

        # This is the command that is used to clean a target before building it,
        # excluding precious targets.
        if sys.platform == "win32":
            rm_cmd = f'cmd.exe /c del /q $rm_outs >nul 2>&1 &'
        else:
            rm_cmd = 'rm -f $rm_outs;'

        precious_rule_suffix = "_PRECIOUS"

        # Make two sets of rules to honor scons Precious setting. The build nodes themselves
        # will then reselect their rule according to the precious being set for that node.
        precious_rules = {}
        for rule, kwargs in self.rules.items():
            if self.env.get('NINJA_MAX_JOBS') is not None and 'pool' not in kwargs:
                kwargs['pool'] = 'local_pool'
            # Do not worry about precious for commands that don't have targets (phony)
            # or that will callback to scons (which maintains its own precious).
            if rule not in ['phony', 'TEMPLATE', 'REGENERATE', 'COMPDB_CC', 'COMPDB_CXX']:
                precious_rule = rule + precious_rule_suffix
                precious_rules[precious_rule] = kwargs.copy()
                ninja.rule(precious_rule, **precious_rules[precious_rule])

                kwargs['command'] = f"{rm_cmd} " + kwargs['command']
                ninja.rule(rule, **kwargs)
            else:

                ninja.rule(rule, **kwargs)
        self.rules.update(precious_rules)

        # If the user supplied an alias to determine generated sources, use that, otherwise
        # determine what the generated sources are dynamically.
        generated_sources_alias = self.env.get('NINJA_GENERATED_SOURCE_ALIAS_NAME')
        generated_sources_build = None

        if generated_sources_alias:
            generated_sources_build = self.builds.get(generated_sources_alias)
            if generated_sources_build is None or generated_sources_build["rule"] != 'phony':
                raise Exception(
                    "ERROR: 'NINJA_GENERATED_SOURCE_ALIAS_NAME' set, but no matching Alias object found."
                )

        if generated_sources_alias and generated_sources_build:
            generated_source_files = sorted(
                [] if not generated_sources_build else generated_sources_build['implicit'])

            def check_generated_source_deps(build):
                return (build != generated_sources_build
                        and set(build["outputs"]).isdisjoint(generated_source_files))
        else:
            generated_sources_build = None
            generated_source_files = sorted({
                output
                # First find builds which have header files in their outputs.
                for build in self.builds.values() if self.has_generated_sources(build["outputs"])
                for output in build["outputs"]
                # Collect only the header files from the builds with them
                # in their output. We do this because is_generated_source
                # returns True if it finds a header in any of the outputs,
                # here we need to filter so we only have the headers and
                # not the other outputs.
                if self.is_generated_source(output)
            })

            if generated_source_files:
                generated_sources_alias = "_ninja_generated_sources"
                ninja_sorted_build(
                    ninja,
                    outputs=generated_sources_alias,
                    rule="phony",
                    implicit=generated_source_files,
                )

                def check_generated_source_deps(build):
                    return (not build["rule"] == "INSTALL"
                            and set(build["outputs"]).isdisjoint(generated_source_files)
                            and set(build.get("implicit", [])).isdisjoint(generated_source_files))

        template_builders = []

        # If we ever change the name/s of the rules that include
        # compile commands (i.e. something like CC) we will need to
        # update this build to reflect that complete list.
        compile_commands = "compile_commands.json"
        compdb_expand = '-x ' if self.env.get('NINJA_COMPDB_EXPAND') else ''
        adjust_script_out = os.path.join(
            get_path(self.env['NINJA_BUILDDIR']), 'ninja_compdb_adjust.py')
        os.makedirs(os.path.dirname(adjust_script_out), exist_ok=True)
        with open(adjust_script_out, 'w') as f:
            f.write(ninja_compdb_adjust)
        self.builds[compile_commands] = {
            'rule': "CMD",
            'outputs': [compile_commands],
            'pool': "console",
            'implicit': [ninja_file],
            'variables': {
                "cmd":
                    f"ninja -f {ninja_file} -t compdb {compdb_expand}COMPDB_CC COMPDB_CXX > {compile_commands};"
                    + f"{sys.executable} {adjust_script_out} {compile_commands}"
            },
        }
        self.builds["compiledb"] = {
            'rule': "phony",
            "outputs": ["compiledb"],
            'implicit': [compile_commands],
        }

        # Now for all build nodes, we want to select the precious rule or not.
        # If it's not precious, we need to save all the outputs into a variable
        # on that node. Later we will be removing outputs and switching them to
        # phonies so that we can generate response and depfiles correctly.
        for build, kwargs in self.builds.items():
            if kwargs.get('variables') and kwargs['variables'].get('precious'):
                kwargs['rule'] = kwargs['rule'] + precious_rule_suffix
            elif kwargs['rule'] not in ['phony', 'TEMPLATE', 'REGENERATE']:
                if not kwargs.get('variables'):
                    kwargs['variables'] = {}
                kwargs['variables']['rm_outs'] = kwargs['outputs'].copy()

        for build in [self.builds[key] for key in sorted(self.builds.keys())]:
            if build["rule"] == "TEMPLATE":
                template_builders.append(build)
                continue

            if "implicit" in build:
                build["implicit"].sort()

            # Don't make generated sources depend on each other. We
            # have to check that none of the outputs are generated
            # sources and none of the direct implicit dependencies are
            # generated sources or else we will create a dependency
            # cycle.
            if (generated_source_files and check_generated_source_deps(build)):

                # Make all non-generated source targets depend on
                # _generated_sources. We use order_only for generated
                # sources so that we don't rebuild the world if one
                # generated source was rebuilt. We just need to make
                # sure that all of these sources are generated before
                # other builds.
                order_only = build.get("order_only", [])
                order_only.append(generated_sources_alias)
                build["order_only"] = order_only
            if "order_only" in build:
                build["order_only"].sort()

            # When using a depfile Ninja can only have a single output
            # but SCons will usually have emitted an output for every
            # thing a command will create because it's caching is much
            # more complex than Ninja's. This includes things like DWO
            # files. Here we make sure that Ninja only ever sees one
            # target when using a depfile. It will still have a command
            # that will create all of the outputs but most targets don't
            # depend direclty on DWO files and so this assumption is safe
            # to make.
            rule = self.rules.get(build["rule"])

            # Some rules like 'phony' and other builtins we don't have
            # listed in self.rules so verify that we got a result
            # before trying to check if it has a deps key.
            #
            # Anything using deps or rspfile in Ninja can only have a single
            # output, but we may have a build which actually produces
            # multiple outputs which other targets can depend on. Here we
            # slice up the outputs so we have a single output which we will
            # use for the "real" builder and multiple phony targets that
            # match the file names of the remaining outputs. This way any
            # build can depend on any output from any build.
            #
            # We assume that the first listed output is the 'key'
            # output and is stably presented to us by SCons. For
            # instance if -gsplit-dwarf is in play and we are
            # producing foo.o and foo.dwo, we expect that outputs[0]
            # from SCons will be the foo.o file and not the dwo
            # file. If instead we just sorted the whole outputs array,
            # we would find that the dwo file becomes the
            # first_output, and this breaks, for instance, header
            # dependency scanning.
            if rule is not None and (rule.get("deps") or rule.get("rspfile")):
                first_output, remaining_outputs = (
                    build["outputs"][0],
                    build["outputs"][1:],
                )

                if remaining_outputs:
                    ninja_sorted_build(
                        ninja,
                        outputs=sorted(remaining_outputs),
                        rule="phony",
                        implicit=first_output,
                    )

                build["outputs"] = first_output

            # Optionally a rule can specify a depfile, and SCons can generate implicit
            # dependencies into the depfile. This allows for dependencies to come and go
            # without invalidating the ninja file. The depfile was created in ninja specifically
            # for dealing with header files appearing and disappearing across rebuilds, but it can
            # be repurposed for anything, as long as you have a way to regenerate the depfile.
            # More specific info can be found here: https://ninja-build.org/manual.html#_depfile
            if rule is not None and rule.get('depfile') and build.get('deps_files'):
                path = build['outputs'] if SCons.Util.is_List(
                    build['outputs']) else [build['outputs']]
                generate_depfile(self.env, path[0], build.pop('deps_files', []))

            if "inputs" in build:
                build["inputs"].sort()

            ninja_sorted_build(ninja, **build)

        for build, kwargs in self.builds.items():
            if kwargs['rule'] in [
                    'CC', f'CC{precious_rule_suffix}', 'CXX', f'CXX{precious_rule_suffix}'
            ]:
                rule = kwargs['rule'].replace(
                    precious_rule_suffix
                ) if precious_rule_suffix in kwargs['rule'] else kwargs['rule']
                rule = "COMPDB_" + rule
                compdb_build = kwargs.copy()

                compdb_build['rule'] = rule
                compdb_build['outputs'] = [kwargs['outputs'] + ".compdb"]
                ninja.build(**compdb_build)

        template_builds = {'rule': "TEMPLATE"}
        for template_builder in template_builders:

            # Special handling for outputs and implicit since we need to
            # aggregate not replace for each builder.
            for agg_key in ["outputs", "implicit", "inputs"]:
                new_val = template_builds.get(agg_key, [])

                # Use pop so the key is removed and so the update
                # below will not overwrite our aggregated values.
                cur_val = template_builder.pop(agg_key, [])
                if is_List(cur_val):
                    new_val += cur_val
                else:
                    new_val.append(cur_val)
                template_builds[agg_key] = new_val

        if template_builds.get("outputs", []):
            ninja_sorted_build(ninja, **template_builds)

        # We have to glob the SCons files here to teach the ninja file
        # how to regenerate itself. We'll never see ourselves in the
        # DAG walk so we can't rely on action_to_ninja_build to
        # generate this rule even though SCons should know we're
        # dependent on SCons files.
        #
        # The REGENERATE rule uses depfile, so we need to generate the depfile
        # in case any of the SConscripts have changed. The depfile needs to be
        # path with in the build and the passed ninja file is an abspath, so
        # we will use SCons to give us the path within the build. Normally
        # generate_depfile should not be called like this, but instead be called
        # through the use of custom rules, and filtered out in the normal
        # list of build generation about. However, because the generate rule
        # is hardcoded here, we need to do this generate_depfile call manually.
        ninja_file_path = self.env.File(ninja_file).path
        ninja_in_file_path = os.path.join(
            get_path(self.env['NINJA_BUILDDIR']), os.path.basename(ninja_file)) + ".in"
        generate_depfile(
            self.env,
            ninja_in_file_path,
            self.env['NINJA_REGENERATE_DEPS'],
        )

        ninja_sorted_build(
            ninja,
            outputs=ninja_in_file_path,
            rule="REGENERATE",
            variables={
                "self": ninja_file_path,
            },
        )

        # This sets up a dependency edge between build.ninja.in and build.ninja
        # without actually taking any action to transform one into the other
        # because we write both files ourselves later.
        ninja_sorted_build(
            ninja,
            outputs=ninja_file_path,
            rule="NOOP",
            inputs=[ninja_in_file_path],
            implicit=[__file__],
        )

        # Look in SCons's list of DEFAULT_TARGETS, find the ones that
        # we generated a ninja build rule for.
        scons_default_targets = [
            get_path(tgt) for tgt in SCons.Script.DEFAULT_TARGETS if get_path(tgt) in self.built
        ]

        # If we found an overlap between SCons's list of default
        # targets and the targets we created ninja builds for then use
        # those as ninja's default as well.
        if scons_default_targets:
            ninja.default(" ".join(scons_default_targets))

        with tempfile.NamedTemporaryFile(delete=False, mode='w') as temp_ninja_file:
            temp_ninja_file.write(content.getvalue())
        shutil.move(temp_ninja_file.name, ninja_file)
        shutil.copy2(ninja_file, ninja_in_file_path)

        self.__generated = True


def get_path(node):
    """
    Return a fake path if necessary.

    As an example Aliases use this as their target name in Ninja.
    """
    if hasattr(node, "get_path"):
        return node.get_path()
    return str(node)


def rfile(node):
    """
    Return the repository file for node if it has one. Otherwise return node
    """
    if hasattr(node, "rfile"):
        return node.rfile()
    return node


def src_file(node):
    """Returns the src code file if it exists."""
    if hasattr(node, "srcnode"):
        src = node.srcnode()
        if src.stat() is not None:
            return src
    return get_path(node)


def get_comstr(env, action, targets, sources):
    """Get the un-substituted string for action."""
    # Despite being having "list" in it's name this member is not
    # actually a list. It's the pre-subst'd string of the command. We
    # use it to determine if the command we're about to generate needs
    # to use a custom Ninja rule. By default this redirects CC, CXX,
    # AR, SHLINK, and LINK commands to their respective rules but the
    # user can inject custom Ninja rules and tie them to commands by
    # using their pre-subst'd string.
    if hasattr(action, "process"):
        return action.cmd_list

    return action.genstring(targets, sources, env)


def ninja_recursive_sorted_dict(build):
    sorted_dict = OrderedDict()
    for key, val in sorted(build.items()):
        if isinstance(val, dict):
            sorted_dict[key] = ninja_recursive_sorted_dict(val)
        elif isinstance(val, list) and key in ('inputs', 'outputs', 'implicit', 'order_only',
                                               'implicit_outputs'):
            sorted_dict[key] = sorted(val)
        else:
            sorted_dict[key] = val
    return sorted_dict


def ninja_sorted_build(ninja, **build):
    sorted_dict = ninja_recursive_sorted_dict(build)
    ninja.build(**sorted_dict)


def get_command_env(env, target, source):
    """
    Return a string that sets the environment for any environment variables that
    differ between the OS environment and the SCons command ENV.

    It will be compatible with the default shell of the operating system.
    """
    try:
        return env["NINJA_ENV_VAR_CACHE"]
    except KeyError:
        pass

    # Scan the ENV looking for any keys which do not exist in
    # os.environ or differ from it. We assume if it's a new or
    # differing key from the process environment then it's
    # important to pass down to commands in the Ninja file.
    ENV = env.get('SHELL_ENV_GENERATOR', get_default_ENV)(env, target, source)
    scons_specified_env = {
        key: value
        for key, value in ENV.items() if key not in os.environ or os.environ.get(key, None) != value
    }

    windows = env["PLATFORM"] == "win32"
    command_env = ""
    for key, value in sorted(scons_specified_env.items()):
        # Ensure that the ENV values are all strings:
        if is_List(value):
            # If the value is a list, then we assume it is a
            # path list, because that's a pretty common list-like
            # value to stick in an environment variable:
            value = flatten_sequence(value)
            value = joinpath(map(str, value))
        else:
            # If it isn't a string or a list, then we just coerce
            # it to a string, which is the proper way to handle
            # Dir and File instances and will produce something
            # reasonable for just about everything else:
            value = str(value)

        if windows:
            command_env += "set '{}={}' && ".format(key, value)
        else:
            # We address here *only* the specific case that a user might have
            # an environment variable which somehow gets included and has
            # spaces in the value. These are escapes that Ninja handles. This
            # doesn't make builds on paths with spaces (Ninja and SCons issues)
            # nor expanding response file paths with spaces (Ninja issue) work.
            value = value.replace(r' ', r'$ ')
            command_env += "export {}='{}';".format(key,
                                                    env.subst(value, target=target, source=source))

    env["NINJA_ENV_VAR_CACHE"] = command_env
    return command_env


def gen_get_response_file_command(env, rule, tool, tool_is_dynamic=False, custom_env=None):
    """Generate a response file command provider for rule name."""

    if custom_env is None:
        custom_env = {}

    # If win32 using the environment with a response file command will cause
    # ninja to fail to create the response file. Additionally since these rules
    # generally are not piping through cmd.exe /c any environment variables will
    # make CreateProcess fail to start.
    #
    # On POSIX we can still set environment variables even for compile
    # commands so we do so.
    use_command_env = not env["PLATFORM"] == "win32"
    if "$" in tool:
        tool_is_dynamic = True

    def get_response_file_command(env, node, action, targets, sources, executor=None):
        if hasattr(action, "process"):
            cmd_list, _, _ = action.process(targets, sources, env, executor=executor)
            cmd_list = [str(c).replace("$", "$$") for c in cmd_list[0]]
        else:
            command = generate_command(env, node, action, targets, sources, executor=executor)
            cmd_list = shlex.split(command)

        if tool_is_dynamic:
            tool_command = env.subst(tool, target=targets, source=sources, executor=executor)
        else:
            tool_command = tool

        try:
            # Add 1 so we always keep the actual tool inside of cmd
            tool_idx = cmd_list.index(tool_command) + 1
        except ValueError:
            raise Exception("Could not find tool {} in {} generated from {}".format(
                tool, cmd_list, get_comstr(env, action, targets, sources)))

        cmd, rsp_content = cmd_list[:tool_idx], cmd_list[tool_idx:]
        rsp_content = " ".join(rsp_content)

        variables = {"rspc": rsp_content}
        variables[rule] = cmd
        if use_command_env:
            variables["env"] = get_command_env(env, targets, sources)

            for key, value in custom_env.items():
                variables["env"] += env.subst(
                    f"export {key}={value};",
                    target=targets,
                    source=sources,
                    executor=executor,
                ) + " "
        return rule, variables, [tool_command]

    return get_response_file_command


def generate_command(env, node, action, targets, sources, executor=None):
    # Actions like CommandAction have a method called process that is
    # used by SCons to generate the cmd_line they need to run. So
    # check if it's a thing like CommandAction and call it if we can.
    if hasattr(action, "process"):
        cmd_list, _, _ = action.process(targets, sources, env, executor=executor)
        cmd = _string_from_cmd_list(cmd_list[0])
    else:
        # Anything else works with genstring, this is most commonly hit by
        # ListActions which essentially call process on all of their
        # commands and concatenate it for us.
        genstring = action.genstring(targets, sources, env)
        if executor is not None:
            cmd = env.subst(genstring, executor=executor)
        else:
            cmd = env.subst(genstring, targets, sources)

        cmd = cmd.replace("\n", " && ").strip()
        if cmd.endswith("&&"):
            cmd = cmd[0:-2].strip()

    # Escape dollars as necessary
    return cmd.replace("$", "$$")


def get_generic_shell_command(env, node, action, targets, sources, executor=None):

    if env.get('NINJA_TEMPLATE'):
        rule = 'TEMPLATE'
    else:
        rule = 'CMD'

    return (
        rule,
        {
            "cmd": generate_command(env, node, action, targets, sources, executor=None),
            "env": get_command_env(env, targets, sources),
        },
        # Since this function is a rule mapping provider, it must return a list of dependencies,
        # and usually this would be the path to a tool, such as a compiler, used for this rule.
        # However this function is to generic to be able to reliably extract such deps
        # from the command, so we return a placeholder empty list. It should be noted that
        # generally this function will not be used soley and is more like a template to generate
        # the basics for a custom provider which may have more specific options for a provier
        # function for a custom NinjaRuleMapping.
        [],
    )


def get_command(env, node, action):
    """Get the command to execute for node."""
    if node.env:
        sub_env = node.env
    else:
        sub_env = env

    executor = node.get_executor()
    if executor is not None:
        tlist = executor.get_all_targets()
        slist = executor.get_all_sources()
    else:
        if hasattr(node, "target_peers"):
            tlist = node.target_peers
        else:
            tlist = [node]
        slist = node.sources

    # Retrieve the repository file for all sources
    slist = [rfile(s) for s in slist]

    # Generate a real CommandAction
    if isinstance(action, SCons.Action.CommandGeneratorAction):
        # pylint: disable=protected-access
        action = action._generate(tlist, slist, sub_env, 0, executor=executor)

    variables = {}

    comstr = get_comstr(sub_env, action, tlist, slist)
    if not comstr:
        return None

    provider = __NINJA_RULE_MAPPING.get(comstr, get_generic_shell_command)
    rule, variables, provider_deps = provider(
        sub_env,
        node,
        action,
        tlist,
        slist,
        executor=executor,
    )

    # Get the dependencies for all targets
    implicit = list({dep for tgt in tlist for dep in get_dependencies(tgt)})

    # Now add in the other dependencies related to the command,
    # e.g. the compiler binary. The ninja rule can be user provided so
    # we must do some validation to resolve the dependency path for ninja.
    for provider_dep in provider_deps:

        provider_dep = sub_env.subst(provider_dep)
        if not provider_dep:
            continue

        # If the tool is a node, then SCons will resolve the path later, if its not
        # a node then we assume it generated from build and make sure it is existing.
        if isinstance(provider_dep, SCons.Node.Node) or os.path.exists(provider_dep):
            implicit.append(provider_dep)
            continue

        # in some case the tool could be in the local directory and be suppled without the ext
        # such as in windows, so append the executable suffix and check.
        prog_suffix = sub_env.get('PROGSUFFIX', '')
        provider_dep_ext = provider_dep if provider_dep.endswith(
            prog_suffix) else provider_dep + prog_suffix
        if os.path.exists(provider_dep_ext):
            implicit.append(provider_dep_ext)
            continue

        # Many commands will assume the binary is in the path, so
        # we accept this as a possible input from a given command.

        provider_dep_abspath = sub_env.WhereIs(provider_dep) or sub_env.WhereIs(
            provider_dep, path=os.environ["PATH"])
        if provider_dep_abspath:
            implicit.append(provider_dep_abspath)
            continue

        # Possibly these could be ignore and the build would still work, however it may not always
        # rebuild correctly, so we hard stop, and force the user to fix the issue with the provided
        # ninja rule.
        err_msg = f"Could not resolve path for '{provider_dep}' dependency on node '{node}', you may need to setup your shell environment for ninja builds."
        if os.name == "nt":
            err_msg += " On Windows, please ensure that you have run the necessary Visual Studio environment setup scripts (e.g. vcvarsall.bat ...,  or launching a Visual Studio Command Prompt) before invoking SCons."
        raise Exception(err_msg)

    ninja_build = {
        "order_only": get_order_only(node),
        "outputs": get_outputs(node),
        "inputs": get_inputs(node),
        "implicit": implicit,
        "rule": rule,
        "variables": variables,
    }

    # Don't use sub_env here because we require that NINJA_POOL be set
    # on a per-builder call basis to prevent accidental strange
    # behavior like env['NINJA_POOL'] = 'console' and sub_env can be
    # the global Environment object if node.env is None.
    # Example:
    #
    # Allowed:
    #
    #     env.Command("ls", NINJA_POOL="ls_pool")
    #
    # Not allowed and ignored:
    #
    #     env["NINJA_POOL"] = "ls_pool"
    #     env.Command("ls")
    #
    if node.env and node.env.get("NINJA_POOL", None) is not None:
        ninja_build["pool"] = node.env["NINJA_POOL"]

    return ninja_build


def ninja_builder(env, target, source):
    """Generate a build.ninja for source."""
    if not isinstance(source, list):
        source = [source]
    if not isinstance(target, list):
        target = [target]

    # We have no COMSTR equivalent so print that we're generating
    # here.
    print("Generating:", str(target[0]))

    generated_build_ninja = target[0].get_abspath()
    NINJA_STATE.generate(generated_build_ninja)

    return 0


# pylint: disable=too-few-public-methods
class AlwaysExecAction(SCons.Action.FunctionAction):
    """Override FunctionAction.__call__ to always execute."""

    def __call__(self, *args, **kwargs):
        kwargs["execute"] = 1
        return super().__call__(*args, **kwargs)


def register_custom_handler(env, name, handler):
    """Register a custom handler for SCons function actions."""
    env[NINJA_CUSTOM_HANDLERS][name] = handler


def register_custom_rule_mapping(env, pre_subst_string, rule):
    """Register a function to call for a given rule."""
    global __NINJA_RULE_MAPPING
    __NINJA_RULE_MAPPING[pre_subst_string] = rule


def register_custom_rule(env, rule, command, description="", deps=None, pool=None,
                         use_depfile=False, use_response_file=False, response_file_content="$rspc",
                         restat=False):
    """Allows specification of Ninja rules from inside SCons files."""
    rule_obj = {
        "command": command,
        "description": description if description else "{} $out".format(rule),
    }

    if use_depfile:
        rule_obj["depfile"] = os.path.join(get_path(env['NINJA_BUILDDIR']), '$out.depfile')

    if deps is not None:
        rule_obj["deps"] = deps

    if pool is not None:
        rule_obj["pool"] = pool

    if use_response_file:
        rule_obj["rspfile"] = "$out.rsp"
        if rule_obj["rspfile"] not in command:
            raise Exception(
                f'Bad Ninja Custom Rule: response file requested, but {rule_obj["rspfile"]} not in in command: {command}'
            )
        rule_obj["rspfile_content"] = response_file_content

    if restat:
        rule_obj["restat"] = 1

    env[NINJA_RULES][rule] = rule_obj


def register_custom_pool(env, pool, size):
    """Allows the creation of custom Ninja pools"""
    env[NINJA_POOLS][pool] = size


def set_build_node_callback(env, node, callback):
    if 'conftest' not in str(node):
        setattr(node.attributes, "ninja_build_callback", callback)


def ninja_csig(original):
    """Return a dummy csig"""

    def wrapper(self):
        name = str(self)
        if "SConscript" in name or "SConstruct" in name:
            return original(self)
        return "dummy_ninja_csig"

    return wrapper


def ninja_contents(original):
    """Return a dummy content without doing IO"""

    def wrapper(self):
        name = str(self)
        if "SConscript" in name or "SConstruct" in name:
            return original(self)
        return bytes("dummy_ninja_contents", encoding="utf-8")

    return wrapper


def CheckNinjaCompdbExpand(env, context):
    """ Configure check testing if ninja's compdb can expand response files"""

    context.Message('Checking if ninja compdb can expand response files... ')
    ret, output = context.TryAction(
        action='ninja -f $SOURCE -t compdb -x CMD_RSP > $TARGET',
        extension='.ninja',
        text=textwrap.dedent("""
            rule CMD_RSP
              command = $cmd @$out.rsp > fake_output.txt
              description = Built $out
              rspfile = $out.rsp
              rspfile_content = $rspc
            build fake_output.txt: CMD_RSP fake_input.txt
              cmd = echo
              pool = console
              rspc = "test"
            """),
    )
    result = '@fake_output.txt.rsp' not in output
    context.Result(result)
    return result


def ninja_stat(_self, path):
    """
    Eternally memoized stat call.

    SCons is very aggressive about clearing out cached values. For our
    purposes everything should only ever call stat once since we're
    running in a no_exec build the file system state should not
    change. For these reasons we patch SCons.Node.FS.LocalFS.stat to
    use our eternal memoized dictionary.
    """
    global NINJA_STAT_MEMO

    try:
        return NINJA_STAT_MEMO[path]
    except KeyError:
        try:
            result = os.stat(path)
        except os.error:
            result = None

        NINJA_STAT_MEMO[path] = result
        return result


def ninja_noop(*_args, **_kwargs):
    """
    A general purpose no-op function.

    There are many things that happen in SCons that we don't need and
    also don't return anything. We use this to disable those functions
    instead of creating multiple definitions of the same thing.
    """
    return None


def ninja_whereis(thing, *_args, **_kwargs):
    """Replace env.WhereIs with a much faster version"""
    global NINJA_WHEREIS_MEMO

    # Optimize for success, this gets called significantly more often
    # when the value is already memoized than when it's not.
    try:
        return NINJA_WHEREIS_MEMO[thing]
    except KeyError:
        # We do not honor any env['ENV'] or env[*] variables in the
        # generated ninja ile. Ninja passes your raw shell environment
        # down to it's subprocess so the only sane option is to do the
        # same during generation. At some point, if and when we try to
        # upstream this, I'm sure a sticking point will be respecting
        # env['ENV'] variables and such but it's actually quite
        # complicated. I have a naive version but making it always work
        # with shell quoting is nigh impossible. So I've decided to
        # cross that bridge when it's absolutely required.
        path = shutil.which(thing)
        NINJA_WHEREIS_MEMO[thing] = path
        return path


def ninja_always_serial(self, num, taskmaster):
    """Replacement for SCons.Job.Jobs constructor which always uses the Serial Job class."""
    # We still set self.num_jobs to num even though it's a lie. The
    # only consumer of this attribute is the Parallel Job class AND
    # the Main.py function which instantiates a Jobs class. It checks
    # if Jobs.num_jobs is equal to options.num_jobs, so if the user
    # provides -j12 but we set self.num_jobs = 1 they get an incorrect
    # warning about this version of Python not supporting parallel
    # builds. So here we lie so the Main.py will not give a false
    # warning to users.
    self.num_jobs = num
    self.job = SCons.Job.Serial(taskmaster)


def ninja_print_conf_log(s, target, source, env):
    """Command line print only for conftest to generate a correct conf log."""
    if target and "conftest" in str(target[0]):
        action = SCons.Action._ActionAction()
        action.print_cmd_line(s, target, source, env)


class NinjaNoResponseFiles(SCons.Platform.TempFileMunge):
    """Overwrite the __call__ method of SCons' TempFileMunge to not delete."""

    def __call__(self, target, source, env, for_signature):
        return self.cmd

    def _print_cmd_str(*_args, **_kwargs):
        """Disable this method"""
        pass


def exists(env):
    """Enable if called."""

    # This variable disables the tool when storing the SCons command in the
    # generated ninja file to ensure that the ninja tool is not loaded when
    # SCons should do actual work as a subprocess of a ninja build. The ninja
    # tool is very invasive into the internals of SCons and so should never be
    # enabled when SCons needs to build a target.
    if env.get("__NINJA_NO", "0") == "1":
        return False

    return True


def generate(env):
    """Generate the NINJA builders."""
    env[NINJA_SYNTAX] = env.get(NINJA_SYNTAX, "ninja_syntax.py")

    # Add the Ninja builder.
    always_exec_ninja_action = AlwaysExecAction(ninja_builder, {})
    ninja_builder_obj = SCons.Builder.Builder(action=always_exec_ninja_action)
    env.Append(BUILDERS={"Ninja": ninja_builder_obj})

    env["NINJA_PREFIX"] = env.get("NINJA_PREFIX", "build")
    env["NINJA_SUFFIX"] = env.get("NINJA_SUFFIX", "ninja")
    env["NINJA_ALIAS_NAME"] = env.get("NINJA_ALIAS_NAME", "generate-ninja")
    env['NINJA_BUILDDIR'] = env.get("NINJA_BUILDDIR", env.Dir(".ninja").path)
    ninja_file_name = env.subst("${NINJA_PREFIX}.${NINJA_SUFFIX}")
    ninja_file = env.Ninja(target=ninja_file_name, source=[])
    env.AlwaysBuild(ninja_file)

    # TODO: API for getting the SConscripts programmatically
    # exists upstream: https://github.com/SCons/scons/issues/3625
    def ninja_generate_deps(env):
        return sorted([env.File("#SConstruct").path] + glob("**/SConscript", recursive=True))

    env['_NINJA_REGENERATE_DEPS_FUNC'] = ninja_generate_deps

    env['NINJA_REGENERATE_DEPS'] = env.get(
        'NINJA_REGENERATE_DEPS',
        '${_NINJA_REGENERATE_DEPS_FUNC(__env__)}',
    )

    # This adds the required flags such that the generated compile
    # commands will create depfiles as appropriate in the Ninja file.
    if env["PLATFORM"] == "win32":
        env.Append(CCFLAGS=["/showIncludes"])
    else:
        env.Append(CCFLAGS=["-MMD", "-MF", "${TARGET}.d"])

    env.AddMethod(CheckNinjaCompdbExpand, "CheckNinjaCompdbExpand")

    # Provide a way for custom rule authors to easily access command
    # generation.
    env.AddMethod(get_generic_shell_command, "NinjaGetGenericShellCommand")
    env.AddMethod(get_command, "NinjaGetCommand")
    env.AddMethod(gen_get_response_file_command, "NinjaGenResponseFileProvider")
    env.AddMethod(set_build_node_callback, "NinjaSetBuildNodeCallback")

    # Expose ninja node path converstion functions to make writing
    # custom function action handlers easier.
    env.AddMethod(lambda _env, node: get_outputs(node), "NinjaGetOutputs")
    env.AddMethod(lambda _env, node, skip_unknown_types=False: get_inputs(node, skip_unknown_types),
                  "NinjaGetInputs")
    env.AddMethod(lambda _env, node, skip_sources=False: get_dependencies(node),
                  "NinjaGetDependencies")
    env.AddMethod(lambda _env, node: get_order_only(node), "NinjaGetOrderOnly")

    # Provides a way for users to handle custom FunctionActions they
    # want to translate to Ninja.
    env[NINJA_CUSTOM_HANDLERS] = {}
    env.AddMethod(register_custom_handler, "NinjaRegisterFunctionHandler")

    # Provides a mechanism for inject custom Ninja rules which can
    # then be mapped using NinjaRuleMapping.
    env[NINJA_RULES] = {}
    env.AddMethod(register_custom_rule, "NinjaRule")

    # Provides a mechanism for inject custom Ninja pools which can
    # be used by providing the NINJA_POOL="name" as an
    # OverrideEnvironment variable in a builder call.
    env[NINJA_POOLS] = {}
    env.AddMethod(register_custom_pool, "NinjaPool")

    # Add the ability to register custom NinjaRuleMappings for Command
    # builders. We don't store this dictionary in the env to prevent
    # accidental deletion of the CC/XXCOM mappings. You can still
    # overwrite them if you really want to but you have to explicit
    # about it this way. The reason is that if they were accidentally
    # deleted you would get a very subtly incorrect Ninja file and
    # might not catch it.
    env.AddMethod(register_custom_rule_mapping, "NinjaRuleMapping")

    # TODO: change LINKCOM and SHLINKCOM to handle embedding manifest exe checks
    # without relying on the SCons hacks that SCons uses by default.
    if env["PLATFORM"] == "win32":
        from SCons.Tool.mslink import compositeLinkAction

        if env["LINKCOM"] == compositeLinkAction:
            env["LINKCOM"] = '${TEMPFILE("$LINK $LINKFLAGS /OUT:$TARGET.windows $_LIBDIRFLAGS $_LIBFLAGS $_PDB $SOURCES.windows", "$LINKCOMSTR")}'
            env["SHLINKCOM"] = '${TEMPFILE("$SHLINK $SHLINKFLAGS $_SHLINK_TARGETS $_LIBDIRFLAGS $_LIBFLAGS $_PDB $_SHLINK_SOURCES", "$SHLINKCOMSTR")}'

    # Normally in SCons actions for the Program and *Library builders
    # will return "${*COM}" as their pre-subst'd command line. However
    # if a user in a SConscript overwrites those values via key access
    # like env["LINKCOM"] = "$( $ICERUN $)" + env["LINKCOM"] then
    # those actions no longer return the "bracketted" string and
    # instead return something that looks more expanded. So to
    # continue working even if a user has done this we map both the
    # "bracketted" and semi-expanded versions.
    def robust_rule_mapping(var, rule, tool):
        provider = gen_get_response_file_command(env, rule, tool)
        env.NinjaRuleMapping("${" + var + "}", provider)
        env.NinjaRuleMapping(env[var], provider)

    robust_rule_mapping("CCCOM", "CC", env["CC"])
    robust_rule_mapping("SHCCCOM", "CC", env["CC"])
    robust_rule_mapping("CXXCOM", "CXX", env["CXX"])
    robust_rule_mapping("SHCXXCOM", "CXX", env["CXX"])
    robust_rule_mapping("LINKCOM", "LINK", "$LINK")
    robust_rule_mapping("SHLINKCOM", "LINK", "$SHLINK")
    robust_rule_mapping("ARCOM", "AR", env["AR"])

    # Make SCons node walk faster by preventing unnecessary work
    env.Decider("timestamp-match")

    # Used to determine if a build generates a source file. Ninja
    # requires that all generated sources are added as order_only
    # dependencies to any builds that *might* use them.
    env["NINJA_GENERATED_SOURCE_SUFFIXES"] = [".h", ".hpp"]

    if env["PLATFORM"] != "win32" and env.get("RANLIBCOM"):
        # There is no way to translate the ranlib list action into
        # Ninja so add the s flag and disable ranlib.
        #
        # This is equivalent to Meson.
        # https://github.com/mesonbuild/meson/blob/master/mesonbuild/linkers.py#L143
        old_arflags = str(env["ARFLAGS"])
        if "s" not in old_arflags:
            old_arflags += "s"

        env["ARFLAGS"] = SCons.Util.CLVar([old_arflags])

        # Disable running ranlib, since we added 's' above
        env["RANLIBCOM"] = ""

    # This is the point of no return, anything after this comment
    # makes changes to SCons that are irreversible and incompatible
    # with a normal SCons build. We return early if __NINJA_NO=1 has
    # been given on the command line (i.e. by us in the generated
    # ninja file) here to prevent these modifications from happening
    # when we want SCons to do work. Everything before this was
    # necessary to setup the builder and other functions so that the
    # tool can be unconditionally used in the users's SCons files.

    if not exists(env):
        return

    # There is a target called generate-ninja which needs to be included
    # with the --ninja flag in order to generate the ninja file. Because the --ninja
    # flag is ONLY used with generate-ninja, we have combined the two by making the --ninja flag
    # implicitly build the generate-ninja target.
    SCons.Script.BUILD_TARGETS = SCons.Script.TargetList(env.Alias("$NINJA_ALIAS_NAME", ninja_file))

    # Set a known variable that other tools can query so they can
    # behave correctly during ninja generation.
    env["GENERATING_NINJA"] = True

    # These methods are no-op'd because they do not work during ninja
    # generation, expected to do no work, or simply fail. All of which
    # are slow in SCons. So we overwrite them with no logic.
    SCons.Node.FS.File.make_ready = ninja_noop
    SCons.Node.FS.File.prepare = ninja_noop
    SCons.Node.FS.File.push_to_cache = ninja_noop
    SCons.Executor.Executor.prepare = ninja_noop
    SCons.Taskmaster.Task.prepare = ninja_noop
    SCons.Node.FS.File.built = ninja_noop
    SCons.Node.Node.visited = ninja_noop

    # We make lstat a no-op because it is only used for SONAME
    # symlinks which we're not producing.
    SCons.Node.FS.LocalFS.lstat = ninja_noop

    # This is a slow method that isn't memoized. We make it a noop
    # since during our generation we will never use the results of
    # this or change the results.
    SCons.Node.FS.is_up_to_date = ninja_noop

    # We overwrite stat and WhereIs with eternally memoized
    # implementations. See the docstring of ninja_stat and
    # ninja_whereis for detailed explanations.
    SCons.Node.FS.LocalFS.stat = ninja_stat
    SCons.Util.WhereIs = ninja_whereis

    # Monkey patch get_csig and get_contents for some classes. It
    # slows down the build significantly and we don't need contents or
    # content signatures calculated when generating a ninja file since
    # we're not doing any SCons caching or building.
    SCons.Executor.Executor.get_contents = ninja_contents(SCons.Executor.Executor.get_contents)
    SCons.Node.Alias.Alias.get_contents = ninja_contents(SCons.Node.Alias.Alias.get_contents)
    SCons.Node.FS.File.get_contents = ninja_contents(SCons.Node.FS.File.get_contents)
    SCons.Node.FS.File.get_csig = ninja_csig(SCons.Node.FS.File.get_csig)
    SCons.Node.FS.Dir.get_csig = ninja_csig(SCons.Node.FS.Dir.get_csig)
    SCons.Node.Alias.Alias.get_csig = ninja_csig(SCons.Node.Alias.Alias.get_csig)

    # Ignore CHANGED_SOURCES and CHANGED_TARGETS. We don't want those
    # to have effect in a generation pass because the generator
    # shouldn't generate differently depending on the current local
    # state. Without this, when generating on Windows, if you already
    # had a foo.obj, you would omit foo.cpp from the response file. Do the same for UNCHANGED.
    SCons.Executor.Executor._get_changed_sources = SCons.Executor.Executor._get_sources
    SCons.Executor.Executor._get_changed_targets = SCons.Executor.Executor._get_targets
    SCons.Executor.Executor._get_unchanged_sources = SCons.Executor.Executor._get_sources
    SCons.Executor.Executor._get_unchanged_targets = SCons.Executor.Executor._get_targets

    # Replace false action messages with nothing.
    env["PRINT_CMD_LINE_FUNC"] = ninja_print_conf_log

    # This reduces unnecessary subst_list calls to add the compiler to
    # the implicit dependencies of targets. Since we encode full paths
    # in our generated commands we do not need these slow subst calls
    # as executing the command will fail if the file is not found
    # where we expect it.
    env["IMPLICIT_COMMAND_DEPENDENCIES"] = False

    # This makes SCons more aggressively cache MD5 signatures in the
    # SConsign file.
    env.SetOption("max_drift", 1)

    # The Serial job class is SIGNIFICANTLY (almost twice as) faster
    # than the Parallel job class for generating Ninja files. So we
    # monkey the Jobs constructor to only use the Serial Job class.
    SCons.Job.Jobs.__init__ = ninja_always_serial

    # The environment variable NINJA_SYNTAX points to the
    # ninja_syntax.py module from the ninja sources found here:
    # https://github.com/ninja-build/ninja/blob/master/misc/ninja_syntax.py
    #
    # This should be vendored into the build sources and it's location
    # set in NINJA_SYNTAX. This code block loads the location from
    # that variable, gets the absolute path to the vendored file, gets
    # it's parent directory then uses importlib to import the module
    # dynamically.
    ninja_syntax_file = env[NINJA_SYNTAX]
    if isinstance(ninja_syntax_file, str):
        ninja_syntax_file = env.File(ninja_syntax_file).get_abspath()
    ninja_syntax_mod_dir = os.path.dirname(ninja_syntax_file)
    sys.path.append(ninja_syntax_mod_dir)
    ninja_syntax_mod_name = os.path.basename(ninja_syntax_file)
    ninja_syntax = importlib.import_module(ninja_syntax_mod_name.replace(".py", ""))

    global NINJA_STATE
    NINJA_STATE = NinjaState(env, ninja_syntax)

    # Here we will force every builder to use an emitter which makes the ninja
    # file depend on it's target. This forces the ninja file to the bottom of
    # the DAG which is required so that we walk every target, and therefore add
    # it to the global NINJA_STATE, before we try to write the ninja file.
    def ninja_file_depends_on_all(target, source, env):
        if not any("conftest" in str(t) for t in target):
            env.Depends(ninja_file, target)
        return target, source

    # The "Alias Builder" isn't in the BUILDERS map so we have to
    # modify it directly.
    SCons.Environment.AliasBuilder.emitter = ninja_file_depends_on_all

    for _, builder in env["BUILDERS"].items():
        try:
            emitter = builder.emitter
            if emitter is not None:
                builder.emitter = SCons.Builder.ListEmitter([
                    emitter,
                    ninja_file_depends_on_all,
                ], )
            else:
                builder.emitter = ninja_file_depends_on_all
        # Users can inject whatever they want into the BUILDERS
        # dictionary so if the thing doesn't have an emitter we'll
        # just ignore it.
        except AttributeError:
            pass

    # Here we monkey patch the Task.execute method to not do a bunch of
    # unnecessary work. If a build is a regular builder (i.e not a conftest and
    # not our own Ninja builder) then we add it to the NINJA_STATE. Otherwise we
    # build it like normal. This skips all of the caching work that this method
    # would normally do since we aren't pulling any of these targets from the
    # cache.
    #
    # In the future we may be able to use this to actually cache the build.ninja
    # file once we have the upstream support for referencing SConscripts as File
    # nodes.
    def ninja_execute(self):
        global NINJA_STATE

        target = self.targets[0]
        target_name = str(target)
        if target_name != ninja_file_name and "conftest" not in target_name:
            NINJA_STATE.add_build(target)
        else:
            target.build()

    SCons.Taskmaster.Task.execute = ninja_execute

    # Make needs_execute always return true instead of determining out of
    # date-ness.
    SCons.Script.Main.BuildTask.needs_execute = lambda x: True

    # We will eventually need to overwrite TempFileMunge to make it
    # handle persistent tempfiles or get an upstreamed change to add
    # some configurability to it's behavior in regards to tempfiles.
    #
    # Set all three environment variables that Python's
    # tempfile.mkstemp looks at as it behaves differently on different
    # platforms and versions of Python.
    os.environ["TMPDIR"] = env.Dir("$BUILD_DIR/response_files").get_abspath()
    os.environ["TEMP"] = os.environ["TMPDIR"]
    os.environ["TMP"] = os.environ["TMPDIR"]
    if not os.path.isdir(os.environ["TMPDIR"]):
        env.Execute(SCons.Defaults.Mkdir(os.environ["TMPDIR"]))

    env["TEMPFILE"] = NinjaNoResponseFiles
