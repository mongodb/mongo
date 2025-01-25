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

import itertools
import json
import os
import shlex
import subprocess
import sys

import SCons

# Implements the ability for SCons to emit a compilation database for the MongoDB project. See
# http://clang.llvm.org/docs/JSONCompilationDatabase.html for details on what a compilation
# database is, and why you might want one. The only user visible entry point here is
# 'env.CompilationDatabase'. This method takes an optional 'target' to name the file that
# should hold the compilation database, otherwise, the file defaults to compile_commands.json,
# which is the name that most clang tools search for by default.

# TODO: Is there a better way to do this than this global? Right now this exists so that the
# emitter we add can record all of the things it emits, so that the scanner for the top level
# compilation database can access the complete list, and also so that the writer has easy
# access to write all of the files. But it seems clunky. How can the emitter and the scanner
# communicate more gracefully?
__COMPILATION_DB_ENTRIES = {}

# Cribbed from Tool/cc.py and Tool/c++.py. It would be better if
# we could obtain this from SCons.
_CSuffixes = [".c"]
if not SCons.Util.case_sensitive_suffixes(".c", ".C"):
    _CSuffixes.append(".C")

_CXXSuffixes = [".cpp", ".cc", ".cxx", ".c++", ".C++"]
if SCons.Util.case_sensitive_suffixes(".c", ".C"):
    _CXXSuffixes.append(".C")


# We make no effort to avoid rebuilding the entries. Someday, perhaps we could and even
# integrate with the cache, but there doesn't seem to be much call for it.
class __CompilationDbNode(SCons.Node.Python.Value):
    def __init__(self, value):
        SCons.Node.Python.Value.__init__(self, value)
        self.Decider(changed_since_last_build_node)


def changed_since_last_build_node(child, target, prev_ni, node):
    """Dummy decider to force always building"""
    return True


def makeEmitCompilationDbEntry(comstr):
    """
    Effectively this creates a lambda function to capture:
    * command line
    * source
    * target
    :param comstr: unevaluated command line
    :return: an emitter which has captured the above
    """

    def EmitCompilationDbEntry(target, source, env):
        """
        This emitter will be added to each c/c++ object build to capture the info needed
        for clang tools
        :param target: target node(s)
        :param source: source node(s)
        :param env: Environment for use building this node
        :return: target(s), source(s)
        """

        dbtarget = __CompilationDbNode(source)

        entry = env.__COMPILATIONDB_Entry(
            target=dbtarget,
            source=[],
            __COMPILATIONDB_UTARGET=target,
            __COMPILATIONDB_USOURCE=source,
            __COMPILATIONDB_COMSTR=comstr,
            __COMPILATIONDB_ENV=env,
        )

        # TODO: Technically, these next two lines should not be required: it should be fine to
        # cache the entries. However, they don't seem to update properly. Since they are quick
        # to re-generate disable caching and sidestep this problem.
        env.AlwaysBuild(entry)
        env.NoCache(entry)

        compiledb_target = env.get("COMPILEDB_TARGET")

        if compiledb_target not in __COMPILATION_DB_ENTRIES:
            __COMPILATION_DB_ENTRIES[compiledb_target] = []

        __COMPILATION_DB_ENTRIES[compiledb_target].append(dbtarget)

        return target, source

    return EmitCompilationDbEntry


def CompilationDbEntryAction(target, source, env, **kw):
    """
    Create a dictionary with evaluated command line, target, source
    and store that info as an attribute on the target
    (Which has been stored in __COMPILATION_DB_ENTRIES array
    :param target: target node(s)
    :param source: source node(s)
    :param env: Environment for use building this node
    :param kw:
    :return: None
    """

    # We will do some surgery on the command line. First we separate the args
    # into a list, then we determine the index of the corresponding compiler
    # value. Then we can extract a list of things before the compiler where are
    # wrappers would be found. We extract the wrapper and put the command back
    # together.
    cmd_list = [
        str(elem)
        for elem in env["__COMPILATIONDB_ENV"].subst_list(
            env["__COMPILATIONDB_COMSTR"],
            target=env["__COMPILATIONDB_UTARGET"],
            source=env["__COMPILATIONDB_USOURCE"],
        )[0]
    ]

    if "CXX" in env["__COMPILATIONDB_COMSTR"]:
        tool_subst = "$CXX"
    else:
        tool_subst = "$CC"
    tool = env["__COMPILATIONDB_ENV"].subst(
        tool_subst, target=env["__COMPILATIONDB_UTARGET"], source=env["__COMPILATIONDB_USOURCE"]
    )

    tool_index = cmd_list.index(tool) + 1
    tool_list = cmd_list[:tool_index]
    cmd_list = cmd_list[tool_index:]

    for wrapper_ignore in env.get("_COMPILATIONDB_IGNORE_WRAPPERS", []):
        wrapper = env.subst(wrapper_ignore, target=target, source=source)
        if wrapper in tool_list:
            tool_list.remove(wrapper)

    tool_abspaths = []
    for tool in tool_list:
        tool_abspath = env.WhereIs(tool)
        if tool_abspath is None:
            tool_abspath = os.path.abspath(str(tool))
        tool_abspaths.append('"' + tool_abspath + '"')
    cmd_list = tool_abspaths + cmd_list

    entry = {
        "directory": env.Dir("#").abspath,
        "command": " ".join(cmd_list),
        "file": str(env["__COMPILATIONDB_USOURCE"][0]),
        "output": shlex.quote(" ".join([str(t) for t in env["__COMPILATIONDB_UTARGET"]])),
    }

    target[0].write(entry)


def WriteCompilationDb(target, source, env):
    entries = []

    for s in __COMPILATION_DB_ENTRIES[target[0].abspath]:
        entries.append(s.read())
    file, ext = os.path.splitext(str(target[0]))
    scons_compdb = f"{file}_scons{ext}"
    with open(scons_compdb, "w") as target_file:
        json.dump(
            entries,
            target_file,
            sort_keys=True,
            indent=4,
            separators=(",", ": "),
        )

    adjust_script_out = env.File("#site_scons/site_tools/compdb_adjust.py").path
    if env.get("COMPDB_IGNORE_BAZEL"):
        bazel_compdb = []
    else:
        bazel_compdb = ["--bazel-compdb", "compile_commands.json"]
        env.RunBazelCommand(
            [env["SCONS2BAZEL_TARGETS"].bazel_executable, "run"]
            + env["BAZEL_FLAGS_STR"]
            + ["//:compiledb", "--"]
            + env["BAZEL_FLAGS_STR"]
        )

    subprocess.run(
        [
            sys.executable,
            adjust_script_out,
            "--input-compdb",
            scons_compdb,
            "--output-compdb",
            str(target[0]),
        ]
        + bazel_compdb
    )


def ScanCompilationDb(node, env, path):
    all_entries = []
    for compiledb_target in __COMPILATION_DB_ENTRIES:
        all_entries.extend(__COMPILATION_DB_ENTRIES[compiledb_target])
    return all_entries


def generate(env, **kwargs):
    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)

    env["COMPILATIONDB_COMSTR"] = kwargs.get(
        "COMPILATIONDB_COMSTR",
        "Building compilation database $TARGET",
    )

    components_by_suffix = itertools.chain(
        itertools.product(
            _CSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$CCCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$SHCCCOM"),
            ],
        ),
        itertools.product(
            _CXXSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$CXXCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$SHCXXCOM"),
            ],
        ),
    )

    for entry in components_by_suffix:
        suffix = entry[0]
        builder, base_emitter, command = entry[1]

        # Assumes a dictionary emitter
        emitter = builder.emitter[suffix]
        builder.emitter[suffix] = SCons.Builder.ListEmitter(
            [
                emitter,
                makeEmitCompilationDbEntry(command),
            ]
        )

    env["BUILDERS"]["__COMPILATIONDB_Entry"] = SCons.Builder.Builder(
        action=SCons.Action.Action(CompilationDbEntryAction, None),
    )

    env["BUILDERS"]["__COMPILATIONDB_Database"] = SCons.Builder.Builder(
        action=SCons.Action.Action(WriteCompilationDb, "$COMPILATIONDB_COMSTR"),
        target_scanner=SCons.Scanner.Scanner(
            function=ScanCompilationDb,
            node_class=None,
        ),
    )

    def CompilationDatabase(env, target):
        result = env.__COMPILATIONDB_Database(target=target, source=[])
        env["COMPILEDB_TARGET"] = result[0].abspath

        env.AlwaysBuild(result)
        env.NoCache(result)

        return result

    env.AddMethod(CompilationDatabase, "CompilationDatabase")


def exists(env):
    return True
