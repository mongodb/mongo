# MIT License
#
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

"""Compilation Database

Implements the ability for SCons to emit a compilation database for a
project. See https://clang.llvm.org/docs/JSONCompilationDatabase.html
for details on what a compilation database is, and why you might want one.
The only user visible entry point here is ``env.CompilationDatabase``.
This method takes an optional *target* to name the file that should hold
the compilation database, otherwise, the file defaults to
``compile_commands.json``, the name that most clang tools search for by default.
"""

import json
import itertools
import fnmatch
import SCons

from SCons.Platform import TempFileMunge

from .cxx import CXXSuffixes
from .cc import CSuffixes
from .asm import ASSuffixes, ASPPSuffixes

DEFAULT_DB_NAME = 'compile_commands.json'

# TODO: Is there a better way to do this than this global? Right now this exists so that the
# emitter we add can record all of the things it emits, so that the scanner for the top level
# compilation database can access the complete list, and also so that the writer has easy
# access to write all of the files. But it seems clunky. How can the emitter and the scanner
# communicate more gracefully?
__COMPILATION_DB_ENTRIES = []


# We make no effort to avoid rebuilding the entries. Someday, perhaps we could and even
# integrate with the cache, but there doesn't seem to be much call for it.
class __CompilationDbNode(SCons.Node.Python.Value):
    def __init__(self, value) -> None:
        SCons.Node.Python.Value.__init__(self, value)
        self.Decider(changed_since_last_build_node)


def changed_since_last_build_node(child, target, prev_ni, node) -> bool:
    """ Dummy decider to force always building"""
    return True


def make_emit_compilation_DB_entry(comstr):
    """
    Effectively this creates a lambda function to capture:
    * command line
    * source
    * target
    :param comstr: unevaluated command line
    :return: an emitter which has captured the above
    """
    user_action = SCons.Action.Action(comstr)

    def emit_compilation_db_entry(target, source, env):
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
            __COMPILATIONDB_UOUTPUT=target,
            __COMPILATIONDB_USOURCE=source,
            __COMPILATIONDB_UACTION=user_action,
            __COMPILATIONDB_ENV=env,
        )

        # TODO: Technically, these next two lines should not be required: it should be fine to
        # cache the entries. However, they don't seem to update properly. Since they are quick
        # to re-generate disable caching and sidestep this problem.
        env.AlwaysBuild(entry)
        env.NoCache(entry)

        __COMPILATION_DB_ENTRIES.append(dbtarget)

        return target, source

    return emit_compilation_db_entry


class CompDBTEMPFILE(TempFileMunge):
    def __call__(self, target, source, env, for_signature):
        return self.cmd


def compilation_db_entry_action(target, source, env, **kw) -> None:
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

    command = env["__COMPILATIONDB_UACTION"].strfunction(
        target=env["__COMPILATIONDB_UOUTPUT"],
        source=env["__COMPILATIONDB_USOURCE"],
        env=env["__COMPILATIONDB_ENV"],
        overrides={'TEMPFILE': CompDBTEMPFILE}
    )

    entry = {
        "directory": env.Dir("#").abspath,
        "command": command,
        "file": env["__COMPILATIONDB_USOURCE"][0],
        "output": env['__COMPILATIONDB_UOUTPUT'][0]
    }

    target[0].write(entry)


def write_compilation_db(target, source, env) -> None:
    entries = []

    use_abspath = env['COMPILATIONDB_USE_ABSPATH'] in [True, 1, 'True', 'true']
    use_path_filter = env.subst('$COMPILATIONDB_PATH_FILTER')

    for s in __COMPILATION_DB_ENTRIES:
        entry = s.read()
        source_file = entry['file']
        output_file = entry['output']

        if use_abspath:
            source_file = source_file.srcnode().abspath
            output_file = output_file.abspath
        else:
            source_file = source_file.srcnode().path
            output_file = output_file.path

        if use_path_filter and not fnmatch.fnmatch(output_file, use_path_filter):
            continue

        path_entry = {'directory': entry['directory'],
                      'command': entry['command'],
                      'file': source_file,
                      'output': output_file}

        entries.append(path_entry)

    with open(target[0].path, "w") as output_file:
        json.dump(
            entries, output_file, sort_keys=True, indent=4, separators=(",", ": ")
        )
        output_file.write("\n")


def scan_compilation_db(node, env, path):
    return __COMPILATION_DB_ENTRIES


def compilation_db_emitter(target, source, env):
    """ fix up the source/targets """

    # Someone called env.CompilationDatabase('my_targetname.json')
    if not target and len(source) == 1:
        target = source

    if not target:
        target = [DEFAULT_DB_NAME]

    # No source should have been passed. Drop it.
    if source:
        source = []

    return target, source


def generate(env, **kwargs) -> None:
    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)

    env["COMPILATIONDB_COMSTR"] = kwargs.get(
        "COMPILATIONDB_COMSTR", "Building compilation database $TARGET"
    )

    components_by_suffix = itertools.chain(
        itertools.product(
            CSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$CCCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$SHCCCOM"),
            ],
        ),
        itertools.product(
            CXXSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$CXXCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$SHCXXCOM"),
            ],
        ),
        itertools.product(
            ASSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$ASCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$ASCOM")
            ],
        ),
        itertools.product(
            ASPPSuffixes,
            [
                (static_obj, SCons.Defaults.StaticObjectEmitter, "$ASPPCOM"),
                (shared_obj, SCons.Defaults.SharedObjectEmitter, "$ASPPCOM")
            ],
        ),
    )

    for entry in components_by_suffix:
        suffix = entry[0]
        builder, base_emitter, command = entry[1]

        # Assumes a dictionary emitter
        emitter = builder.emitter.get(suffix, False)
        if emitter:
            # We may not have tools installed which initialize all or any of
            # cxx, cc, or assembly. If not skip resetting the respective emitter.
            builder.emitter[suffix] = SCons.Builder.ListEmitter(
                [emitter, make_emit_compilation_DB_entry(command), ]
            )

    env["BUILDERS"]["__COMPILATIONDB_Entry"] = SCons.Builder.Builder(
        action=SCons.Action.Action(compilation_db_entry_action, None),
    )

    env["BUILDERS"]["CompilationDatabase"] = SCons.Builder.Builder(
        action=SCons.Action.Action(write_compilation_db, "$COMPILATIONDB_COMSTR"),
        target_scanner=SCons.Scanner.Scanner(
            function=scan_compilation_db, node_class=None
        ),
        emitter=compilation_db_emitter,
        suffix='json',
    )

    env['COMPILATIONDB_USE_ABSPATH'] = False
    env['COMPILATIONDB_PATH_FILTER'] = ''


def exists(env) -> bool:
    return True
