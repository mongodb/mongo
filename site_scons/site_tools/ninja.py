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
from glob import glob

import SCons
from SCons.Action import _string_from_cmd_list, get_default_ENV
from SCons.Script import COMMAND_LINE_TARGETS

NINJA_SYNTAX = "NINJA_SYNTAX"
NINJA_CUSTOM_HANDLERS = "__NINJA_CUSTOM_HANDLERS"
NINJA_ALIAS_PREFIX = "ninja-"
NINJA_CMD = "NINJA_CMD"

# These are the types that get_command can do something with
COMMAND_TYPES = (SCons.Action.CommandAction, SCons.Action.CommandGeneratorAction)

# Global Rules (like SCons Builders) that should always be present.
#
# Primarily we only use "cmd" and pass it the SCons generated compile
# commands.
RULES = {
    "cmd": {"command": "$cmd"},
    "INSTALL": {"command": "$COPY $in $out", "description": "INSTALL $out"},
}

# Global variables that should always be present
VARS = {"COPY": "cmd /c copy" if sys.platform == "win32" else "cp"}


class FakePath:
    """Make nodes with no get_path method work for dependency finding."""

    def __init__(self, node):
        self.node = node

    def get_path(self):
        """Return a fake path, as an example Aliases use this as their target name in Ninja."""
        return str(self.node)


def pathable(node):
    """If node is in the build path return it's source file."""
    if hasattr(node, "get_path"):
        return node
    return FakePath(node)


def rfile(n):
    if hasattr(n, "rfile"):
        return n.rfile()
    return n


def src_file(node):
    """Returns the src code file if it exists."""
    if hasattr(node, "srcnode"):
        src = node.srcnode()
        if src.stat() is not None:
            return src
    return pathable(node)


def get_command(env, node, action):
    """Get the command to execute for node."""
    if node.env:
        sub_env = node.env
    else:
        sub_env = env

    # SCons should not generate resource files since Ninja will
    # need to handle the long commands itself.
    sub_env["MAXLINELENGTH"] = 10000000000000

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
        action = action._generate(tlist, slist, sub_env, 1, executor=executor)

    # Actions like CommandAction have a method called process that is
    # used by SCons to generate the cmd_line they need to run. So
    # check if it's a thing like CommandAction and call it if we can.
    if hasattr(action, "process"):
        cmd_list, _, _ = action.process(tlist, slist, sub_env, executor=executor)
        if cmd_list:
            return _string_from_cmd_list(cmd_list[0])

    # Anything else works with genstring, this is most commonly hit by
    # ListActions which essentially call process on all of their
    # commands and concatenate it for us.
    genstring = action.genstring(tlist, slist, sub_env)
    genstring = genstring.replace("\n", " && ").strip()
    if genstring.endswith("&&"):
        genstring = genstring[0:-2].strip()
    if executor is not None:
        cmd = sub_env.subst(genstring, executor=executor)
    else:
        cmd = sub_env.subst(genstring, target=tlist, source=slist)

    return cmd



def action_to_ninja_build(env, node, action, dependencies):
    """Generate a build arguments for action."""

    if isinstance(node, SCons.Node.Alias.Alias):
        alias_name = str(node)

        # Any alias that starts with NINJA_ALIAS_PREFIX builds another
        # ninja file and we do not want to add those to the generated
        # ninja files.
        if not alias_name.startswith(NINJA_ALIAS_PREFIX):
            return {
                "outputs": str(node),
                "rule": "phony",
                "implicit": [
                    src_file(n).get_path() for n in node.all_children()
                    if not str(n).startswith(NINJA_ALIAS_PREFIX)
                ],
            }

    # Ideally this should never happen, and we do try to filter
    # Ninja builders out of being sources of ninja builders but I
    # can't fix every DAG problem so we just skip ninja_builders
    # if we find one
    elif node.builder == env["BUILDERS"]["Ninja"]:
        return None

    elif isinstance(action, SCons.Action.FunctionAction):
        name = action.function_name()
        if name == "installFunc":
            return {
                "outputs": node.get_path(),
                "rule": "INSTALL",
                "inputs": [src_file(s).get_path() for s in node.sources],
                "implicit": dependencies,
            }
        else:
            custom_handler = env[NINJA_CUSTOM_HANDLERS].get(name, None)
            if custom_handler is not None and callable(custom_handler):
                print("Using custom handler {} for {}".format(custom_handler.__name__, str(node)))
                return custom_handler
            else:
                # This is the reported name for Substfile and Textfile
                # which we still want SCons to generate so don't warn
                # for it.
                if name != "_action":
                    print(
                        "Found unhandled function action {}, "
                        " generating scons command to build".format(name)
                    )
                    print(
                        "Note: this is less efficient than Ninja,"
                        " you can write your own ninja build generator for"
                        " this function using NinjaRegisterFunctionHandler"
                    )
                return (node, dependencies)

    elif isinstance(action, COMMAND_TYPES):
        cmd = getattr(node.attributes, NINJA_CMD, None)

        # If cmd is None it was a node that we didn't hit during print
        # time. Ideally everything get hits at that point because the
        # most SCons state is still available however, some things do
        # not work at this stage so we fall back to trying again here
        # in the builder.
        if cmd is None:
            print("Generating Ninja action for", str(node))
            cmd = get_command(env, node, action)

        return {
            "outputs": node.get_path(),
            "rule": "cmd",
            "variables": {"cmd": cmd},
            "implicit": dependencies,
        }

    elif isinstance(action, SCons.Action.LazyAction):
        generated = action._generate_cache(node.env if node.env else env)
        return action_to_ninja_build(env, node, generated, dependencies)

    # If nothing else works make scons generate the file
    return (node, dependencies)


def handle_action(env, ninja, node, action, ninja_file=None):
    """
    Write a Ninja build for the node's action type.

    Returns a tuple of node and dependencies if it couldn't create a Ninja build for it.
    """
    children = node.all_children()
    dependencies = [src_file(n).get_path() for n in children]
    if ninja_file is not None:
        dependencies.append(ninja_file)

    build = None
    if isinstance(action, SCons.Action.ListAction):
        results = []
        for act in action.list:
            results.append(action_to_ninja_build(env, node, act, dependencies))

        # Filter out empty cmdline nodes
        results = [r for r in results if r["rule"] == "cmd" and r["variables"]["cmd"]]
        if len(results) == 1:
            build = results[0]

        all_outputs = [cmd["outputs"] for cmd in results] 
        # If we have no outputs we're done
        if not all_outputs:
            return None

        # Used to verify if all rules are the same
        all_install = [r for r in results if isinstance(r, dict) and r["rule"] == "INSTALL"]
        all_phony = [r for r in results if isinstance(r, dict) and r["rule"] == "phony"]
        all_commands = [r for r in results if isinstance(r, dict) and r["rule"] == "cmd"]

        if all_commands and len(all_commands) == len(results):
            cmdline = ""
            for cmd in all_commands:
                if cmdline != "":
                    cmdline += " && "

                if cmd["variables"]["cmd"]:
                    cmdline += cmd["variables"]["cmd"]

            # Make sure we didn't generate an empty cmdline
            if cmdline:
                build = {
                    "outputs": all_outputs,
                    "rule": "cmd",
                    "variables": {"cmd": cmdline},
                    "implicit": dependencies,
                }

        elif all_phony and len(all_phony) == len(results):
            build = {
                "outputs": all_outputs,
                "rule": "phony",
                "implicit": dependencies,
            }

        elif all_install and len(all_install) == len(results):
            build = {
                "outputs": all_outputs,
                "rule": "INSTALL",
                "inputs": dependencies,
            }
            
        else:
            # Else use scons to generate these outputs
            build = {
                "outputs": all_outputs,
                "rule": "SCONSGEN",
                "implicit": dependencies,
            }
    else:
        build = action_to_ninja_build(env, node, action, dependencies)

    # If action_to_ninja_build returns a tuple that means SCons is required
    if isinstance(build, tuple):
        return build
    # If action_to_ninja_build returns a function it's a custom Function handler
    elif callable(build):
        build(env, ninja, node, dependencies)
    elif build is not None:
        ninja.build(**build)

    return None


def handle_exec_node(env, ninja, node, built, scons_required=None, ninja_file=None):
    """Write a ninja build rule for node and it's children."""
    if scons_required is None:
        scons_required = []

    if node in built:
        return scons_required

    # Probably a source file and so no work needs to be done, it will
    # be used in the generation for some real target.
    if getattr(node, "builder", None) is None:
        return scons_required

    action = node.builder.action
    scons_cmd = handle_action(env, ninja, node, action, ninja_file=ninja_file)
    if scons_cmd is not None:
        scons_required.append(scons_cmd)

    built.add(node)
    for child in node.all_children():
        scons_required += handle_exec_node(env, ninja, child, built, ninja_file=ninja_file)

    return scons_required


def handle_build_node(env, ninja, node, ninja_file=None):
    """Write a ninja build rule for node."""
    # Since the SCons graph looks more like an octopus than a
    # Christmas tree we need to make sure we only built nodes once
    # with Ninja or else Ninja will be upset we have duplicate
    # targets.
    built = set()

    # handle_exec_node returns a list of tuples of the form (node,
    # [stringified_dependencies]) for any node it didn't know what to
    # do with. So we will generate scons commands to build those
    # files. This is often (and intended) to be Substfile and Textfile
    # calls.
    scons_required = handle_exec_node(env, ninja, node, built, ninja_file=ninja_file)

    # Since running SCons is expensive we try to de-duplicate
    # Substfile / Textfile calls to make as few SCons calls from
    # Ninja as possible.
    #
    # TODO: attempt to dedupe non-Substfile/Textfile calls
    combinable_outputs = []
    combinable_dependencies = []
    for target in scons_required:
        builder = target[0].builder
        if builder in (env["BUILDERS"]["Substfile"], env["BUILDERS"]["Textfile"]):
            if hasattr(target[0], "target_peers"):
                combinable_outputs.extend([str(s) for s in target[0].target_peers])
            combinable_outputs.append(str(target[0]))
            combinable_dependencies.extend(target[1])
        else:
            ninja.build(str(target[0]), rule="SCONS", implicit=list(set(target[1])))

    if combinable_outputs:
        ninja.build(
            list(set(combinable_outputs)),
            rule="SCONS",
            implicit=list(set(combinable_dependencies)),
        )


def ninja_builder(env, target, source):
    """Generate a build.ninja for source."""
    if not isinstance(source, list):
        source = [source]
    if not isinstance(target, list):
        target = [target]

    # Only build if we're building the alias that's actually been
    # asked for so we don't generate a bunch of build.ninja files.
    alias_name = str(source[0])
    ninja_alias_name = "{}{}".format(NINJA_ALIAS_PREFIX, alias_name)
    if ninja_alias_name not in COMMAND_LINE_TARGETS:
        return 0
    else:
        print("Generating:", str(target[0]))

    ninja_syntax_file = env[NINJA_SYNTAX]
    if isinstance(ninja_syntax_file, str):
        ninja_syntax_file = env.File(ninja_syntax_file).get_abspath()
    ninja_syntax_mod_dir = os.path.dirname(ninja_syntax_file)
    sys.path.append(ninja_syntax_mod_dir)
    ninja_syntax_mod_name = os.path.basename(ninja_syntax_file)
    ninja_syntax = importlib.import_module(ninja_syntax_mod_name.replace(".py", ""))

    generated_build_ninja = target[0].get_abspath()

    content = io.StringIO()
    ninja = ninja_syntax.Writer(content, width=100)
    ninja.comment("Generated by scons for target {}. DO NOT EDIT.".format(alias_name))
    ninja.comment("vim: set textwidth=0 :")
    ninja.comment("-*- eval: (auto-fill-mode -1) -*-")

    ninja.variable("PYTHON", sys.executable)
    ninja.variable(
        "SCONS_INVOCATION",
        "{} {} -Q $out".format(
            sys.executable,
            " ".join(
                [
                    arg for arg in sys.argv

                    # TODO: the "ninja" in arg part of this should be
                    # handled better, as it stands this is for
                    # filtering out MongoDB's ninja flag
                    if arg not in COMMAND_LINE_TARGETS and "ninja" not in arg
                ]
            ),
        ),
    )

    ninja.variable(
        "SCONS_INVOCATION_W_TARGETS",
        "$SCONS_INVOCATION {}".format(
            " ".join([arg for arg in sys.argv if arg in COMMAND_LINE_TARGETS])
        ),
    )

    ninja.rule(
        "SCONS",
        command="$SCONS_INVOCATION $out",
        description="SCONSGEN $out",

        # restat
        #    if present, causes Ninja to re-stat the command’s outputs
        #    after execution of the command. Each output whose
        #    modification time the command did not change will be
        #    treated as though it had never needed to be built. This
        #    may cause the output’s reverse dependencies to be removed
        #    from the list of pending build actions.
        restat=1,
    )

    for rule in RULES:
        ninja.rule(rule, **RULES[rule])

    for var, val in VARS.items():
        ninja.variable(var, val)

    environ = get_default_ENV(env)
    for var, val in environ.items():
        ninja.variable(var, val)

    for src in source:
        handle_build_node(env, ninja, src, ninja_file=generated_build_ninja)

    ninja.build(
        generated_build_ninja,
        rule="SCONS",
        implicit=glob("**SCons*", recursive=True),
    )

    ninja.build(
        "scons-invocation",
        rule="cmd",
        pool="console",
        variables={"cmd": "echo $SCONS_INVOCATION_W_TARGETS"},
    )

    ninja.default(pathable(source[0]).get_path())

    with open(generated_build_ninja, "w", encoding="utf-8") as build_ninja:
        build_ninja.write(content.getvalue())

    build_ninja_file = env.File("#build.ninja")
    build_ninja_path = build_ninja_file.get_abspath()
    print("Linking build.ninja to {}".format(generated_build_ninja))
    if os.path.islink(build_ninja_path):
        os.remove(build_ninja_path)
    os.symlink(generated_build_ninja, build_ninja_path)
    return 0


class AlwaysExecAction(SCons.Action.FunctionAction):
    """Override FunctionAction.__call__ to always execute."""

    def __call__(self, *args, **kwargs):
        kwargs["execute"] = 1
        return super().__call__(*args, **kwargs)


def ninja_disguise(alias_func):
    """Wrap Alias with a ninja 'emitter'."""
    seen = set()

    def ninja_alias_wrapper(env, target, source=None, action=None, **kw):
        """Call the original alias_func and generate a Ninja builder call."""
        alias_name = env.subst(str(target))
        alias = alias_func(env, alias_name, source, action, **kw)
        if not alias_name.startswith(NINJA_ALIAS_PREFIX) and not alias_name in seen:
            alias_func(
                env,
                NINJA_ALIAS_PREFIX + alias_name,
                env.Ninja(target="#{}.build.ninja${{NINJA_SUFFIX}}".format(alias_name), source=alias),
            )
            seen.add(alias_name)

        return alias

    return ninja_alias_wrapper


def ninja_print(_cmd, target, _source, env):
    """Print an accurate generation message and tag the targets with the commands to build them."""
    if target:
        for tgt in target:
            if (
                    tgt.builder is not None and
                    isinstance(tgt.builder.action, COMMAND_TYPES) and
                    getattr(tgt.attributes, NINJA_CMD, None) is None
            ):
                print("Generating Ninja action for", str(tgt))
                setattr(tgt.attributes, NINJA_CMD, get_command(env, tgt, tgt.builder.action))
            elif getattr(tgt.attributes, NINJA_CMD, None) is None:
                print("Deferring Ninja generation for", str(tgt))
    return 0


def register_custom_handler(env, name, handler):
    """Register a custom handler for SCons function actions."""
    env[NINJA_CUSTOM_HANDLERS][name] = handler


def ninja_decider(*args, **kwargs):
    """Ninja decider skips all calculation in the decision step."""
    return False


def exists(_env):
    """Enable if called."""
    return True


def generate(env):
    """Generate the NINJA builders."""
    env[NINJA_SYNTAX] = env.get(NINJA_SYNTAX, "ninja_syntax.py")

    # Add the Ninja builder. This really shouldn't be called by users
    # but I guess it could be and will probably Do The Right Thing™
    NinjaAction = AlwaysExecAction(ninja_builder, {})
    Ninja = SCons.Builder.Builder(action=NinjaAction)
    env.Append(BUILDERS={"Ninja": Ninja})

    # Provides a way for users to handle custom FunctionActions they
    # want to translate to Ninja.
    env[NINJA_CUSTOM_HANDLERS] = {}
    env.AddMethod(register_custom_handler, "NinjaRegisterFunctionHandler")

    # Make SCons node walk faster by preventing unnecessary work
    env.Decider(ninja_decider)
    env.SetOption("max_drift", 1)

    # Monkey Patch Alias to use our custom wrapper that generates a
    # "hidden" internal alias that corresponds to what the user
    # actually passed as an action / source and then generates a new
    # alias matching the user provided alias name that just calls the
    # ninja builder for that alias.
    SCons.Environment.Base.Alias = ninja_disguise(SCons.Environment.Base.Alias)

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
