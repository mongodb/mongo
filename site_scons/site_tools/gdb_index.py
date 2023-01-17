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

import SCons


def _update_builder(env, builder):

    verbose = '' if env.Verbose() else None

    base_action = builder.action
    if not isinstance(base_action, SCons.Action.ListAction):
        base_action = SCons.Action.ListAction([base_action])

    # There are cases were a gdb-index file is NOT generated from gdb 'save gdb-index' command,
    # mostly shim libraries where there is no code in the library, and the following Actions would
    # then fail. The files are created make sure there is always a file to operate on, if its an
    # empty file then the following actions are basically NOOP, and its cleaner then writing
    # conditions into each action.

    # Because this is all taking under one task, the list action will always run all actions if
    # the target is out of date. So the gdb-index files would always be regenerated, and there is
    # no value in keeping them around, it will just waste disk space. Therefore they should be
    # removed as if they never existed from the task. The build system doesn't need to know about
    # them.
    if env.get('DWARF_VERSION') <= 4:
        base_action.list.extend([
            SCons.Action.Action(
                'touch ${TARGET}.gdb-index',
                verbose,
            ),
            SCons.Action.Action(
                '$GDB --batch-silent --quiet --nx --eval-command "save gdb-index ${TARGET.dir}" $TARGET',
                "$GDB_INDEX_GEN_INDEX_STR",
            ),
            SCons.Action.Action(
                '$OBJCOPY --add-section .gdb_index=${TARGET}.gdb-index --set-section-flags .gdb_index=readonly ${TARGET} ${TARGET}',
                "$GDB_INDEX_ADD_SECTION_STR",
            ),
            SCons.Action.Action(
                'rm -f ${TARGET}.gdb-index',
                verbose,
            ),
        ])
    else:
        base_action.list.extend([
            SCons.Action.Action(
                'touch ${TARGET}.debug_names ${TARGET}.debug_str',
                verbose,
            ),
            SCons.Action.Action(
                '$GDB --batch-silent --quiet --nx --eval-command "save gdb-index -dwarf-5 ${TARGET.dir}" $TARGET',
                "$GDB_INDEX_GEN_INDEX_STR",
            ),
            SCons.Action.Action(
                '$OBJCOPY --dump-section .debug_str=${TARGET}.debug_str.new $TARGET',
                verbose,
            ),
            SCons.Action.Action(
                'cat ${TARGET}.debug_str >>${TARGET}.debug_str.new',
                verbose,
            ),
            SCons.Action.Action(
                '$OBJCOPY --add-section .debug_names=${TARGET}.debug_names --set-section-flags .debug_names=readonly --update-section .debug_str=${TARGET}.debug_str.new ${TARGET} ${TARGET}',
                "$GDB_INDEX_ADD_SECTION_STR",
            ),
            SCons.Action.Action(
                'rm -f ${TARGET}.debug_names ${TARGET}.debug_str.new ${TARGET}.debug_str',
                verbose,
            ),
        ])

    builder.action = base_action


def generate(env):
    if env.get("OBJCOPY", None) is None:
        env["OBJCOPY"] = env.WhereIs("objcopy")
    if env.get("GDB", None) is None:
        env["GDB"] = env.WhereIs("gdb")

    if not env.Verbose():
        env.Append(
            GDB_INDEX_GEN_INDEX_STR="Using $GDB to generate index for $TARGET",
            GDB_INDEX_ADD_SECTION_STR="Adding index sections into $TARGET",
        )

    for builder in ["Program", "SharedLibrary", "LoadableModule"]:
        _update_builder(env, env["BUILDERS"][builder])


def exists(env):
    result = False
    if env.TargetOSIs("posix"):
        objcopy = env.get("OBJCOPY", None) or env.WhereIs("objcopy")
        gdb = env.get("GDB", None) or env.WhereIs("gdb")
        try:
            dwarf_version = int(env.get('DWARF_VERSION'))
        except ValueError:
            dwarf_version = None

        unset_vars = []
        if not objcopy:
            unset_vars += ['OBJCOPY']
        if not gdb:
            unset_vars += ['GDB']
        if not dwarf_version:
            unset_vars += ['DWARF_VERSION']

        if not unset_vars:
            print("Enabled generation of gdb index into binaries.")
            result = True
        else:
            print(f"Disabled generation gdb index because {', '.join(unset_vars)} were not set.")
    return result
