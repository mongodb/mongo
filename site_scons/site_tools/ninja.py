# Copyright 2019 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generate build.ninja files from SCons aliases."""

import sys
import os
import importlib
import io
import shutil
from glob import glob

import SCons
from SCons.Action import _string_from_cmd_list
from SCons.Script import COMMAND_LINE_TARGETS

NINJA_SYNTAX = "NINJA_SYNTAX"
NINJA_RULES = "__NINJA_CUSTOM_RULES"
NINJA_CUSTOM_HANDLERS = "__NINJA_CUSTOM_HANDLERS"
NINJA_BUILD = "NINJA_BUILD"
NINJA_OUTPUTS = "__NINJA_OUTPUTS"

# These are the types that get_command can do something with
COMMAND_TYPES = (
    SCons.Action.CommandAction,
    SCons.Action.CommandGeneratorAction,
)


# TODO: make this configurable or improve generated source detection
def is_generated_source(output):
    """
    Determine if output indicates this is a generated header file.
    """
    for generated in output:
        if generated.endswith(".h") or generated.endswith(".hpp"):
            return True
    return False


def null_function(_env, _node):
    """Do nothing for this function action"""
    return None


def _install_action_function(_env, node):
    """Install files using the install or copy commands"""
    return {
        "outputs": get_outputs(node),
        "rule": "INSTALL",
        "pool": "install_pool",
        "inputs": [get_path(src_file(s)) for s in node.sources],
        "implicit": get_dependencies(node),
    }


def _lib_symlink_action_function(_env, node):
    """Create shared object symlinks if any need to be created"""
    symlinks = getattr(getattr(node, "attributes", None), "shliblinks", None)

    if not symlinks or symlinks is None:
        return None

    outputs = [link.get_dir().rel_path(linktgt) for link, linktgt in symlinks]
    if getattr(node.attributes, NINJA_OUTPUTS, None) is None:
        setattr(node.attributes, NINJA_OUTPUTS, outputs)

    inputs = [link.get_path() for link, _ in symlinks]

    return {
        "outputs": outputs,
        "inputs": inputs,
        "rule": "SYMLINK",
        "implicit": get_dependencies(node),
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
    return not isinstance(node, SCons.Node.Alias.Alias) or node.children()


def alias_to_ninja_build(node):
    """Convert an Alias node into a Ninja phony target"""
    return {
        "outputs": get_outputs(node),
        "rule": "phony",
        "implicit": [
            get_path(n) for n in node.children() if is_valid_dependent_node(n)
        ],
    }


def get_dependencies(node):
    """Return a list of dependencies for node."""
    # TODO: test if this is faster as a try except
    deps = getattr(node.attributes, "NINJA_DEPS", None)
    if deps is None:
        deps = [get_path(src_file(child)) for child in node.children()]
        setattr(node.attributes, "NINJA_DEPS", deps)
    return deps


def get_outputs(node):
    """Collect the Ninja outputs for node."""
    outputs = getattr(node.attributes, NINJA_OUTPUTS, None)
    if outputs is None:
        executor = node.get_executor()
        if executor is not None:
            outputs = executor.get_all_targets()
        else:
            if hasattr(node, "target_peers"):
                outputs = node.target_peers
            else:
                outputs = [node]
        outputs = [get_path(o) for o in outputs]
        setattr(node.attributes, NINJA_OUTPUTS, outputs)
    return outputs


class SConsToNinjaTranslator:
    """Translates SCons Actions into Ninja build objects."""

    def __init__(self, env):
        self.env = env
        self.func_handlers = {

            # Skip conftest builders
            "_createSource": null_function,

            # SCons has a custom FunctionAction that just makes sure the
            # target isn't static. We let the commands that ninja runs do
            # this check for us.
            "SharedFlagChecker": null_function,

            # The install builder is implemented as a function action.
            "installFunc": _install_action_function,
            "LibSymlinksActionFunction": _lib_symlink_action_function,

        }

        self.func_handlers.update(self.env[NINJA_CUSTOM_HANDLERS])

    # pylint: disable=too-many-return-statements
    def action_to_ninja_build(self, node, action=None):
        """Generate build arguments dictionary for node."""
        # Use False since None is a valid value for this Attribute
        build = getattr(node.attributes, NINJA_BUILD, False)
        if build is not False:
            return build

        if node.builder is None:
            return None

        if action is None:
            action = node.builder.action

        # Ideally this should never happen, and we do try to filter
        # Ninja builders out of being sources of ninja builders but I
        # can't fix every DAG problem so we just skip ninja_builders
        # if we find one
        if node.builder == self.env["BUILDERS"]["Ninja"]:
            return None

        if isinstance(action, SCons.Action.FunctionAction):
            return self.handle_func_action(node, action)

        if isinstance(action, SCons.Action.LazyAction):
            # pylint: disable=protected-access
            action = action._generate_cache(node.env if node.env else self.env)
            return self.action_to_ninja_build(node, action=action)

        if isinstance(action, SCons.Action.ListAction):
            return self.handle_list_action(node, action)

        if isinstance(action, COMMAND_TYPES):
            return get_command(node.env if node.env else self.env, node, action)

        # Return the node to indicate that SCons is required
        return {
            "rule": "SCONS",
            "outputs": get_outputs(node),
            "implicit": get_dependencies(node),
        }

    def handle_func_action(self, node, action):
        """Determine how to handle the function action."""
        name = action.function_name()
        # This is the name given by the Subst/Textfile builders. So return the
        # node to indicate that SCons is required
        if name == "_action":
            return {
                "rule": "TEMPLATE",
                "outputs": get_outputs(node),
                "implicit": get_dependencies(node),
            }

        handler = self.func_handlers.get(name, None)
        if handler is not None:
            return handler(node.env if node.env else self.env, node)

        print(
            "Found unhandled function action {}, "
            " generating scons command to build\n"
            "Note: this is less efficient than Ninja,"
            " you can write your own ninja build generator for"
            " this function using NinjaRegisterFunctionHandler".format(name)
        )

        return {
            "rule": "SCONS",
            "outputs": get_outputs(node),
            "implicit": get_dependencies(node),
        }

    # pylint: disable=too-many-branches
    def handle_list_action(self, node, action):
        """TODO write this comment"""
        results = [
            self.action_to_ninja_build(node, action=act)
            for act in action.list
            if act is not None
        ]
        results = [result for result in results if result is not None]
        if not results:
            return None

        # No need to process the results if we only got a single result
        if len(results) == 1:
            return results[0]

        all_outputs = list({output for build in results for output in build["outputs"]})
        setattr(node.attributes, NINJA_OUTPUTS, all_outputs)
        # If we have no outputs we're done
        if not all_outputs:
            return None

        # Used to verify if all rules are the same
        all_one_rule = len(
            [
                r
                for r in results
                if isinstance(r, dict) and r["rule"] == results[0]["rule"]
            ]
        ) == len(results)
        dependencies = get_dependencies(node)

        if not all_one_rule:
            # If they aren't all the same rule use scons to generate these
            # outputs. At this time nothing hits this case.
            return {
                "outputs": all_outputs,
                "rule": "SCONS",
                "implicit": dependencies,
            }

        if results[0]["rule"] == "CMD":
            cmdline = ""
            for cmd in results:
                if not cmd["variables"]["cmd"]:
                    continue

                if cmdline:
                    cmdline += " && "

                # Skip duplicate commands
                if cmd["variables"]["cmd"] in cmdline:
                    continue

                cmdline += cmd["variables"]["cmd"]

            # Remove all preceding and proceeding whitespace
            cmdline = cmdline.strip()

            # Make sure we didn't generate an empty cmdline
            if cmdline:
                return {
                    "outputs": all_outputs,
                    "rule": "CMD",
                    "variables": {"cmd": cmdline},
                    "implicit": dependencies,
                }

        elif results[0]["rule"] == "phony":
            return {
                "outputs": all_outputs,
                "rule": "phony",
                "implicit": dependencies,
            }

        elif results[0]["rule"] == "INSTALL":
            return {
                "outputs": all_outputs,
                "rule": "INSTALL",
                "pool": "install_pool",
                "inputs": [get_path(src_file(s)) for s in node.sources],
                "implicit": dependencies,
            }

        elif results[0]["rule"] == "SCONS":
            return {
                "outputs": all_outputs,
                "rule": "SCONS",
                "inputs": dependencies,
            }

        raise Exception("Unhandled list action with rule: " + results[0]["rule"])


# pylint: disable=too-many-instance-attributes
class NinjaState:
    """Maintains state of Ninja build system as it's translated from SCons."""

    def __init__(self, env, writer_class):
        self.env = env
        self.writer_class = writer_class
        self.__generated = False
        self.translator = SConsToNinjaTranslator(env)
        self.builds = list()

        # SCons sets this variable to a function which knows how to do
        # shell quoting on whatever platform it's run on. Here we use it
        # to make the SCONS_INVOCATION variable properly quoted for things
        # like CCFLAGS
        escape = env.get("ESCAPE", lambda x: x)

        self.built = set()
        self.variables = {
            "COPY": "cmd.exe /c copy" if sys.platform == "win32" else "cp",
            "SCONS_INVOCATION": "{} {} __NINJA_NO=1 $out".format(
                sys.executable,
                " ".join(
                    [escape(arg) for arg in sys.argv if arg not in COMMAND_LINE_TARGETS]
                ),
            ),
            "SCONS_INVOCATION_W_TARGETS": "{} {}".format(
                sys.executable,
                " ".join([escape(arg) for arg in sys.argv])
            ),
        }

        self.rules = {
            "CMD": {
                "command": "cmd /c $cmd" if sys.platform == "win32" else "$cmd",
                "description": "Building $out",
            },
            "SYMLINK": {
                "command": (
                    "cmd /c mklink $out $in"
                    if sys.platform == "win32"
                    else "ln -s $in $out"
                ),
                "description": "symlink $in -> $out",
            },
            "INSTALL": {"command": "$COPY $in $out", "description": "Install $out"},
            "TEMPLATE": {
                "command": "$SCONS_INVOCATION $out",
                "description": "Rendering $out",
                # Console pool restricts to 1 job running at a time,
                # it additionally has some special handling about
                # passing stdin, stdout, etc to process in this pool
                # that we need for SCons to behave correctly when run
                # by Ninja.
                "pool": "console",
                "restat": 1,
            },
            "SCONS": {
                "command": "$SCONS_INVOCATION $out",
                "description": "SCons $out",
                "pool": "console",
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
                "description": "Regenerating $out",
                "pool": "console",
                # Again we restat in case Ninja thought the
                # build.ninja should be regenerated but SCons knew
                # better.
                "restat": 1,
            },
        }
        self.rules.update(env.get(NINJA_RULES, {}))

    def generate_builds(self, node):
        """Generate a ninja build rule for node and it's children."""
        # Filter out nodes with no builder. They are likely source files
        # and so no work needs to be done, it will be used in the
        # generation for some real target.
        #
        # Note that all nodes have a builder attribute but it is sometimes
        # set to None. So we cannot use a simpler hasattr check here.
        if getattr(node, "builder", None) is None:
            return

        stack = [[node]]
        self.built = set()

        while stack:
            frame = stack.pop()
            for child in frame:
                outputs = set(get_outputs(child))
                # Check if all the outputs are in self.built, if they
                # are we've already seen this node and it's children.
                if not outputs.isdisjoint(self.built):
                    continue

                self.built = self.built.union(outputs)
                stack.append(child.children())

                if isinstance(child, SCons.Node.Alias.Alias):
                    build = alias_to_ninja_build(child)
                elif node.builder is not None:
                    # Use False since None is a valid value for this attribute
                    build = getattr(child.attributes, NINJA_BUILD, False)
                    if build is False:
                        print("Generating Ninja build for:", str(child))
                        build = self.translator.action_to_ninja_build(child)
                        setattr(child.attributes, NINJA_BUILD, build)
                else:
                    build = None

                # Some things are unbuild-able or need not be built in Ninja
                if build is None or build == 0:
                    continue

                print("Collecting build for:", build["outputs"])
                self.builds.append(build)

    # pylint: disable=too-many-branches,too-many-locals
    def generate(self, ninja_file, fallback_default_target=None):
        """
        Generate the build.ninja.

        This should only be called once for the lifetime of this object.
        """
        if self.__generated:
            return

        content = io.StringIO()
        ninja = self.writer_class(content, width=100)

        ninja.comment("Generated by scons. DO NOT EDIT.")

        ninja.pool("install_pool", self.env.GetOption("num_jobs") / 2)

        for var, val in self.variables.items():
            ninja.variable(var, val)

        for rule, kwargs in self.rules.items():
            ninja.rule(rule, **kwargs)

        generated_source_files = {
            output

            # First find builds which have header files in their outputs.
            for build in self.builds
            if is_generated_source(build["outputs"])
            for output in build["outputs"]

            # Collect only the header files from the builds with them
            # in their output. We do this because is_generated_source
            # returns True if it finds a header in any of the outputs,
            # here we need to filter so we only have the headers and
            # not the other outputs.
            if output.endswith(".h") or output.endswith(".hpp")
        }

        if generated_source_files:
            ninja.build(
                outputs="_generated_sources",
                rule="phony",
                implicit=list(generated_source_files),
            )

        template_builders = []

        for build in self.builds:
            if build["rule"] == "TEMPLATE":
                template_builders.append(build)
                continue

            implicit = build.get("implicit", [])
            implicit.append(ninja_file)
            build["implicit"] = implicit

            # Don't make generated sources depend on each other. We
            # have to check that none of the outputs are generated
            # sources and none of the direct implicit dependencies are
            # generated sources or else we will create a dependency
            # cycle.
            if (
                not build["rule"] == "INSTALL"
                and not is_generated_source(build["outputs"])
                and set(implicit).isdisjoint(generated_source_files)
            ):

                # Make all non-generated source targets depend on
                # _generated_sources. We use order_only for generated
                # sources so that we don't rebuild the world if one
                # generated source was rebuilt. We just need to make
                # sure that all of these sources are generated before
                # other builds.
                build["order_only"] = "_generated_sources"

            ninja.build(**build)

        template_builds = dict()
        for template_builder in template_builders:

            # Special handling for outputs and implicit since we need to
            # aggregate not replace for each builder.
            for agg_key in ["outputs", "implicit"]:
                new_val = template_builds.get(agg_key, [])

                # Use pop so the key is removed and so the update
                # below will not overwrite our aggregated values.
                cur_val = template_builder.pop(agg_key, [])
                if isinstance(cur_val, list):
                    new_val += cur_val
                else:
                    new_val.append(cur_val)
                template_builds[agg_key] = new_val

            # Collect all other keys
            template_builds.update(template_builder)

        if template_builds.get("outputs", []):
            ninja.build(**template_builds)

        # We have to glob the SCons files here to teach the ninja file
        # how to regenerate itself. We'll never see ourselves in the
        # DAG walk so we can't rely on action_to_ninja_build to
        # generate this rule even though SCons should know we're
        # dependent on SCons files.
        ninja.build(
            ninja_file, rule="REGENERATE", implicit=glob("**/SCons*", recursive=True),
        )

        ninja.build(
            "scons-invocation",
            rule="CMD",
            pool="console",
            variables={"cmd": "echo $SCONS_INVOCATION_W_TARGETS"},
        )

        # Look in SCons's list of DEFAULT_TARGETS, find the ones that
        # we generated a ninja build rule for.
        scons_default_targets = [
            get_path(tgt)
            for tgt in SCons.Script.DEFAULT_TARGETS
            if get_path(tgt) in self.built
        ]

        # If we found an overlap between SCons's list of default
        # targets and the targets we created ninja builds for then use
        # those as ninja's default as well.
        if scons_default_targets:
            ninja.default(" ".join(scons_default_targets))

        # If not then set the default to the fallback_default_target we were given.
        # Otherwise we won't create a default ninja target.
        elif fallback_default_target is not None:
            ninja.default(fallback_default_target)

        with open(ninja_file, "w") as build_ninja:
            build_ninja.write(content.getvalue())

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


# TODO: Make the Rules smarter. Instead of just using a "cmd" rule
# everywhere we should be smarter about generating CC, CXX, LINK,
# etc. rules
def get_command(env, node, action):  # pylint: disable=too-many-branches
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

    # Get the dependencies for all targets
    implicit = list({dep for tgt in tlist for dep in get_dependencies(tgt)})

    # Generate a real CommandAction
    if isinstance(action, SCons.Action.CommandGeneratorAction):
        # pylint: disable=protected-access
        action = action._generate(tlist, slist, sub_env, 1, executor=executor)

    # Actions like CommandAction have a method called process that is
    # used by SCons to generate the cmd_line they need to run. So
    # check if it's a thing like CommandAction and call it if we can.
    if hasattr(action, "process"):
        cmd_list, _, _ = action.process(tlist, slist, sub_env, executor=executor)
        cmd = _string_from_cmd_list(cmd_list[0])
    else:
        # Anything else works with genstring, this is most commonly hit by
        # ListActions which essentially call process on all of their
        # commands and concatenate it for us.
        genstring = action.genstring(tlist, slist, sub_env)
        if executor is not None:
            cmd = sub_env.subst(genstring, executor=executor)
        else:
            cmd = sub_env.subst(genstring, target=tlist, source=slist)

        # Since we're only enabling Ninja for developer builds right
        # now we skip all Manifest related work on Windows as it's not
        # necessary. We shouldn't have gotten here but on Windows
        # SCons has a ListAction which shows as a
        # CommandGeneratorAction for linking. That ListAction ends
        # with a FunctionAction (embedManifestExeCheck,
        # embedManifestDllCheck) that simply say "does
        # target[0].manifest exist?" if so execute the real command
        # action underlying me, otherwise do nothing.
        #
        # Eventually we'll want to find a way to translate this to
        # Ninja but for now, and partially because the existing Ninja
        # generator does so, we just disable it all together.
        cmd = cmd.replace("\n", " && ").strip()
        if env["PLATFORM"] == "win32" and ("embedManifestExeCheck" in cmd or "embedManifestDllCheck" in cmd):
            cmd = " && ".join(cmd.split(" && ")[0:-1])

        if cmd.endswith("&&"):
            cmd = cmd[0:-2].strip()

    return {
        "outputs": get_outputs(node),
        "implicit": implicit,
        "rule": "CMD",
        "variables": {"cmd": cmd},
    }


def ninja_builder(env, target, source):
    """Generate a build.ninja for source."""
    if not isinstance(source, list):
        source = [source]
    if not isinstance(target, list):
        target = [target]

    # We have no COMSTR equivalent so print that we're generating
    # here.
    print("Generating:", str(target[0]))

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

    generated_build_ninja = target[0].get_abspath() + env.get("NINJA_SUFFIX", "")
    ninja_state = NinjaState(env, ninja_syntax.Writer)

    for src in source:
        ninja_state.generate_builds(src)

    ninja_state.generate(generated_build_ninja, str(source[0]))

    return 0


# pylint: disable=too-few-public-methods
class AlwaysExecAction(SCons.Action.FunctionAction):
    """Override FunctionAction.__call__ to always execute."""

    def __call__(self, *args, **kwargs):
        kwargs["execute"] = 1
        return super().__call__(*args, **kwargs)


def ninja_print(_cmd, target, _source, env):
    """Tag targets with the commands to build them."""
    if target:
        for tgt in target:
            if (
                tgt.builder is not None
                # Use 'is False' because not would still trigger on
                # None's which we don't want to regenerate
                and getattr(tgt.attributes, NINJA_BUILD, False) is False
                and isinstance(tgt.builder.action, COMMAND_TYPES)
            ):
                ninja_action = get_command(env, tgt, tgt.builder.action)
                setattr(tgt.attributes, NINJA_BUILD, ninja_action)
                # Preload the attributes dependencies while we're still running
                # multithreaded
                get_dependencies(tgt)
    return 0


def register_custom_handler(env, name, handler):
    """Register a custom handler for SCons function actions."""
    env[NINJA_CUSTOM_HANDLERS][name] = handler


def register_custom_rule(env, rule, command, description=""):
    """Allows specification of Ninja rules from inside SCons files."""
    env[NINJA_RULES][rule] = {
        "command": command,
        "description": description if description else "{} $out".format(rule),
    }


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


def ninja_csig(original):
    """Return a dummy csig"""

    def wrapper(self):
        name = str(self)
        if 'SConscript' in name or 'SConstruct' in name:
            return original(self)
        return "dummy_ninja_csig"

    return wrapper


def ninja_contents(original):
    """Return a dummy content without doing IO"""

    def wrapper(self):
        name = str(self)
        if 'SConscript' in name or 'SConstruct' in name:
            return original(self)
        return bytes("dummy_ninja_contents", encoding="utf-8")

    return wrapper


def ninja_noop(*_args, **_kwargs):
    """
    A general purpose no-op function.

    There are many things that happen in SCons that we don't need and
    also don't return anything. We use this to disable those functions
    instead of creating multiple definitions of the same thing.
    """
    pass  # pylint: disable=unnecessary-pass


def ninja_whereis(thing, *_args, **_kwargs):
    """Replace env.WhereIs with a much faster version"""
    return shutil.which(thing)


def generate(env):
    """Generate the NINJA builders."""
    if not exists(env):
        return

    env[NINJA_SYNTAX] = env.get(NINJA_SYNTAX, "ninja_syntax.py")

    # Add the Ninja builder.
    always_exec_ninja_action = AlwaysExecAction(ninja_builder, {})
    ninja_builder_obj = SCons.Builder.Builder(action=always_exec_ninja_action)
    env.Append(BUILDERS={"Ninja": ninja_builder_obj})

    # Provides a way for users to handle custom FunctionActions they
    # want to translate to Ninja.
    env[NINJA_CUSTOM_HANDLERS] = {}
    env.AddMethod(register_custom_handler, "NinjaRegisterFunctionHandler")

    env[NINJA_RULES] = {}
    env.AddMethod(register_custom_rule, "NinjaRule")

    # Make SCons node walk faster by preventing unnecessary work
    env.Decider("timestamp-match")

    # Monkey patch get_csig for some node classes. It slows down the build
    # significantly and we don't need content signatures calculated when
    # generating a ninja file.
    SCons.Node.FS.File.make_ready = ninja_noop
    SCons.Node.FS.File.prepare = ninja_noop
    SCons.Node.FS.File.push_to_cache = ninja_noop
    SCons.Node.FS.File.built = ninja_noop

    SCons.Executor.Executor.get_contents = ninja_contents(SCons.Executor.Executor.get_contents)
    SCons.Node.Alias.Alias.get_contents = ninja_contents(SCons.Node.Alias.Alias.get_contents)
    SCons.Node.FS.File.get_contents = ninja_contents(SCons.Node.FS.File.get_contents)

    SCons.Node.FS.File.get_csig = ninja_csig(SCons.Node.FS.File.get_csig)
    SCons.Node.FS.Dir.get_csig = ninja_csig(SCons.Node.FS.Dir.get_csig)
    SCons.Node.Alias.Alias.get_csig = ninja_csig(SCons.Node.Alias.Alias.get_csig)

    SCons.Util.WhereIs = ninja_whereis

    # pylint: disable=protected-access
    SCons.Platform.TempFileMunge._print_cmd_str = ninja_noop

    # Replace false Compiling* messages with a more accurate output
    #
    # We also use this to tag all Nodes with Builders using
    # CommandActions with the final command that was used to compile
    # it for passing to Ninja. If we don't inject this behavior at
    # this stage in the build too much state is lost to generate the
    # command at the actual ninja_builder execution time for most
    # commands.
    #
    # We do attempt command generation again in ninja_builder if it
    # hasn't been tagged and it seems to work for anything that
    # doesn't represent as a non-FunctionAction during the print_func
    # call.
    env["PRINT_CMD_LINE_FUNC"] = ninja_print

    # Set build to no_exec, our sublcass of FunctionAction will force
    # an execution for ninja_builder so this simply effects all other
    # Builders.
    env.SetOption("no_exec", True)

    # This makes SCons more aggressively cache MD5 signatures in the
    # SConsign file.
    env.SetOption("max_drift", 1)

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

    # Force the SConsign to be written, we benefit from SCons caching of
    # implicit dependencies and conftests. Unfortunately, we have to do this
    # using an atexit handler because SCons will not write the file when in a
    # no_exec build.
    import atexit

    atexit.register(SCons.SConsign.write)
