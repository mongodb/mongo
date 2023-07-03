#!/usr/bin/env python3
# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ##########################################################################

import argparse
import contextlib
import os
import re
import shutil
import sys
from typing import Optional


INCLUDED_SUBDIRS = ["common", "compress", "decompress"]

SKIPPED_FILES = [
    "common/mem.h",
    "common/zstd_deps.h",
    "common/pool.c",
    "common/pool.h",
    "common/threading.c",
    "common/threading.h",
    "common/zstd_trace.h",
    "compress/zstdmt_compress.h",
    "compress/zstdmt_compress.c",
]

XXHASH_FILES = [
    "common/xxhash.c",
    "common/xxhash.h",
]


class FileLines(object):
    def __init__(self, filename):
        self.filename = filename
        with open(self.filename, "r") as f:
            self.lines = f.readlines()

    def write(self):
        with open(self.filename, "w") as f:
            f.write("".join(self.lines))


class PartialPreprocessor(object):
    """
    Looks for simple ifdefs and ifndefs and replaces them.
    Handles && and ||.
    Has fancy logic to handle translating elifs to ifs.
    Only looks for macros in the first part of the expression with no
    parens.
    Does not handle multi-line macros (only looks in first line).
    """
    def __init__(self, defs: [(str, Optional[str])], replaces: [(str, str)], undefs: [str]):
        MACRO_GROUP = r"(?P<macro>[a-zA-Z_][a-zA-Z_0-9]*)"
        ELIF_GROUP = r"(?P<elif>el)?"
        OP_GROUP = r"(?P<op>&&|\|\|)?"

        self._defs = {macro:value for macro, value in defs}
        self._replaces = {macro:value for macro, value in replaces}
        self._defs.update(self._replaces)
        self._undefs = set(undefs)

        self._define = re.compile(r"\s*#\s*define")
        self._if = re.compile(r"\s*#\s*if")
        self._elif = re.compile(r"\s*#\s*(?P<elif>el)if")
        self._else = re.compile(r"\s*#\s*(?P<else>else)")
        self._endif = re.compile(r"\s*#\s*endif")

        self._ifdef = re.compile(fr"\s*#\s*if(?P<not>n)?def {MACRO_GROUP}\s*")
        self._if_defined = re.compile(
            fr"\s*#\s*{ELIF_GROUP}if\s+(?P<not>!)?\s*defined\s*\(\s*{MACRO_GROUP}\s*\)\s*{OP_GROUP}"
        )
        self._if_defined_value = re.compile(
            fr"\s*#\s*{ELIF_GROUP}if\s+defined\s*\(\s*{MACRO_GROUP}\s*\)\s*"
            fr"(?P<op>&&)\s*"
            fr"(?P<openp>\()?\s*"
            fr"(?P<macro2>[a-zA-Z_][a-zA-Z_0-9]*)\s*"
            fr"(?P<cmp>[=><!]+)\s*"
            fr"(?P<value>[0-9]*)\s*"
            fr"(?P<closep>\))?\s*"
        )
        self._if_true = re.compile(
            fr"\s*#\s*{ELIF_GROUP}if\s+{MACRO_GROUP}\s*{OP_GROUP}"
        )

        self._c_comment = re.compile(r"/\*.*?\*/")
        self._cpp_comment = re.compile(r"//")

    def _log(self, *args, **kwargs):
        print(*args, **kwargs)

    def _strip_comments(self, line):
        # First strip c-style comments (may include //)
        while True:
            m = self._c_comment.search(line)
            if m is None:
                break
            line = line[:m.start()] + line[m.end():]

        # Then strip cpp-style comments
        m = self._cpp_comment.search(line)
        if m is not None:
            line = line[:m.start()]

        return line

    def _fixup_indentation(self, macro, replace: [str]):
        if len(replace) == 0:
            return replace
        if len(replace) == 1 and self._define.match(replace[0]) is None:
            # If there is only one line, only replace defines
            return replace


        all_pound = True
        for line in replace:
            if not line.startswith('#'):
                all_pound = False
        if all_pound:
            replace = [line[1:] for line in replace]

        min_spaces = len(replace[0])
        for line in replace:
            spaces = 0
            for i, c in enumerate(line):
                if c != ' ':
                    # Non-preprocessor line ==> skip the fixup
                    if not all_pound and c != '#':
                        return replace
                    spaces = i
                    break
            min_spaces = min(min_spaces, spaces)

        replace = [line[min_spaces:] for line in replace]

        if all_pound:
            replace = ["#" + line for line in replace]

        return replace

    def _handle_if_block(self, macro, idx, is_true, prepend):
        """
        Remove the #if or #elif block starting on this line.
        """
        REMOVE_ONE = 0
        KEEP_ONE = 1
        REMOVE_REST = 2

        if is_true:
            state = KEEP_ONE
        else:
            state = REMOVE_ONE

        line = self._inlines[idx]
        is_if = self._if.match(line) is not None
        assert is_if or self._elif.match(line) is not None
        depth = 0

        start_idx = idx

        idx += 1
        replace = prepend
        finished = False
        while idx < len(self._inlines):
            line = self._inlines[idx]
            # Nested if statement
            if self._if.match(line):
                depth += 1
                idx += 1
                continue
            # We're inside a nested statement
            if depth > 0:
                if self._endif.match(line):
                    depth -= 1
                idx += 1
                continue

            # We're at the original depth

            # Looking only for an endif.
            # We've found a true statement, but haven't
            # completely elided the if block, so we just
            # remove the remainder.
            if state == REMOVE_REST:
                if self._endif.match(line):
                    if is_if:
                        # Remove the endif because we took the first if
                        idx += 1
                    finished = True
                    break
                idx += 1
                continue

            if state == KEEP_ONE:
                m = self._elif.match(line)
                if self._endif.match(line):
                    replace += self._inlines[start_idx + 1:idx]
                    idx += 1
                    finished = True
                    break
                if self._elif.match(line) or self._else.match(line):
                    replace += self._inlines[start_idx + 1:idx]
                    state = REMOVE_REST
                idx += 1
                continue

            if state == REMOVE_ONE:
                m = self._elif.match(line)
                if m is not None:
                    if is_if:
                        idx += 1
                        b = m.start('elif')
                        e = m.end('elif')
                        assert e - b == 2
                        replace.append(line[:b] + line[e:])
                    finished = True
                    break
                m = self._else.match(line)
                if m is not None:
                    if is_if:
                        idx += 1
                        while self._endif.match(self._inlines[idx]) is None:
                            replace.append(self._inlines[idx])
                            idx += 1
                        idx += 1
                    finished = True
                    break
                if self._endif.match(line):
                    if is_if:
                        # Remove the endif because no other elifs
                        idx += 1
                    finished = True
                    break
                idx += 1
                continue
        if not finished:
            raise RuntimeError("Unterminated if block!")

        replace = self._fixup_indentation(macro, replace)

        self._log(f"\tHardwiring {macro}")
        if start_idx > 0:
            self._log(f"\t\t  {self._inlines[start_idx - 1][:-1]}")
        for x in range(start_idx, idx):
            self._log(f"\t\t- {self._inlines[x][:-1]}")
        for line in replace:
            self._log(f"\t\t+ {line[:-1]}")
        if idx < len(self._inlines):
            self._log(f"\t\t  {self._inlines[idx][:-1]}")

        return idx, replace

    def _preprocess_once(self):
        outlines = []
        idx = 0
        changed = False
        while idx < len(self._inlines):
            line = self._inlines[idx]
            sline = self._strip_comments(line)
            m = self._ifdef.fullmatch(sline)
            if_true = False
            if m is None:
                m = self._if_defined_value.fullmatch(sline)
            if m is None:
                m = self._if_defined.match(sline)
            if m is None:
                m = self._if_true.match(sline)
                if_true = (m is not None)
            if m is None:
                outlines.append(line)
                idx += 1
                continue

            groups = m.groupdict()
            macro = groups['macro']
            op = groups.get('op')

            if not (macro in self._defs or macro in self._undefs):
                outlines.append(line)
                idx += 1
                continue

            defined = macro in self._defs

            # Needed variables set:
            # resolved: Is the statement fully resolved?
            # is_true: If resolved, is the statement true?
            ifdef = False
            if if_true:
                if not defined:
                    outlines.append(line)
                    idx += 1
                    continue

                defined_value = self._defs[macro]
                is_int = True
                try:
                    defined_value = int(defined_value)
                except TypeError:
                    is_int = False
                except ValueError:
                    is_int = False

                resolved = is_int
                is_true = (defined_value != 0)

                if resolved and op is not None:
                    if op == '&&':
                        resolved = not is_true
                    else:
                        assert op == '||'
                        resolved = is_true

            else:
                ifdef = groups.get('not') is None
                elseif = groups.get('elif') is not None

                macro2 = groups.get('macro2')
                cmp = groups.get('cmp')
                value = groups.get('value')
                openp = groups.get('openp')
                closep = groups.get('closep')

                is_true = (ifdef == defined)
                resolved = True
                if op is not None:
                    if op == '&&':
                        resolved = not is_true
                    else:
                        assert op == '||'
                        resolved = is_true

                if macro2 is not None and not resolved:
                    assert ifdef and defined and op == '&&' and cmp is not None
                    # If the statement is true, but we have a single value check, then
                    # check the value.
                    defined_value = self._defs[macro]
                    are_ints = True
                    try:
                        defined_value = int(defined_value)
                        value = int(value)
                    except TypeError:
                        are_ints = False
                    except ValueError:
                        are_ints = False
                    if (
                            macro == macro2 and
                            ((openp is None) == (closep is None)) and
                            are_ints
                    ):
                        resolved = True
                        if cmp == '<':
                            is_true = defined_value < value
                        elif cmp == '<=':
                            is_true = defined_value <= value
                        elif cmp == '==':
                            is_true = defined_value == value
                        elif cmp == '!=':
                            is_true = defined_value != value
                        elif cmp == '>=':
                            is_true = defined_value >= value
                        elif cmp == '>':
                            is_true = defined_value > value
                        else:
                            resolved = False

                if op is not None and not resolved:
                    # Remove the first op in the line + spaces
                    if op == '&&':
                        opre = op
                    else:
                        assert op == '||'
                        opre = r'\|\|'
                    needle = re.compile(fr"(?P<if>\s*#\s*(el)?if\s+).*?(?P<op>{opre}\s*)")
                    match = needle.match(line)
                    assert match is not None
                    newline = line[:match.end('if')] + line[match.end('op'):]

                    self._log(f"\tHardwiring partially resolved {macro}")
                    self._log(f"\t\t- {line[:-1]}")
                    self._log(f"\t\t+ {newline[:-1]}")

                    outlines.append(newline)
                    idx += 1
                    continue

            # Skip any statements we cannot fully compute
            if not resolved:
                outlines.append(line)
                idx += 1
                continue

            prepend = []
            if macro in self._replaces:
                assert not ifdef
                assert op is None
                value = self._replaces.pop(macro)
                prepend = [f"#define {macro} {value}\n"]

            idx, replace = self._handle_if_block(macro, idx, is_true, prepend)
            outlines += replace
            changed = True

        return changed, outlines

    def preprocess(self, filename):
        with open(filename, 'r') as f:
            self._inlines = f.readlines()
        changed = True
        iters = 0
        while changed:
            iters += 1
            changed, outlines = self._preprocess_once()
            self._inlines = outlines

        with open(filename, 'w') as f:
            f.write(''.join(self._inlines))


class Freestanding(object):
    def __init__(
            self, zstd_deps: str, mem: str, source_lib: str, output_lib: str,
            external_xxhash: bool, xxh64_state: Optional[str],
            xxh64_prefix: Optional[str], rewritten_includes: [(str, str)],
            defs: [(str, Optional[str])], replaces: [(str, str)],
            undefs: [str], excludes: [str], seds: [str], spdx: bool,
    ):
        self._zstd_deps = zstd_deps
        self._mem = mem
        self._src_lib = source_lib
        self._dst_lib = output_lib
        self._external_xxhash = external_xxhash
        self._xxh64_state = xxh64_state
        self._xxh64_prefix = xxh64_prefix
        self._rewritten_includes = rewritten_includes
        self._defs = defs
        self._replaces = replaces
        self._undefs = undefs
        self._excludes = excludes
        self._seds = seds
        self._spdx = spdx

    def _dst_lib_file_paths(self):
        """
        Yields all the file paths in the dst_lib.
        """
        for root, dirname, filenames in os.walk(self._dst_lib):
            for filename in filenames:
                filepath = os.path.join(root, filename)
                yield filepath

    def _log(self, *args, **kwargs):
        print(*args, **kwargs)

    def _copy_file(self, lib_path):
        suffixes = [".c", ".h", ".S"]
        if not any((lib_path.endswith(suffix) for suffix in suffixes)):
            return
        if lib_path in SKIPPED_FILES:
            self._log(f"\tSkipping file: {lib_path}")
            return
        if self._external_xxhash and lib_path in XXHASH_FILES:
            self._log(f"\tSkipping xxhash file: {lib_path}")
            return

        src_path = os.path.join(self._src_lib, lib_path)
        dst_path = os.path.join(self._dst_lib, lib_path)
        self._log(f"\tCopying: {src_path} -> {dst_path}")
        shutil.copyfile(src_path, dst_path)

    def _copy_source_lib(self):
        self._log("Copying source library into output library")

        assert os.path.exists(self._src_lib)
        os.makedirs(self._dst_lib, exist_ok=True)
        self._copy_file("zstd.h")
        self._copy_file("zstd_errors.h")
        for subdir in INCLUDED_SUBDIRS:
            src_dir = os.path.join(self._src_lib, subdir)
            dst_dir = os.path.join(self._dst_lib, subdir)

            assert os.path.exists(src_dir)
            os.makedirs(dst_dir, exist_ok=True)

            for filename in os.listdir(src_dir):
                lib_path = os.path.join(subdir, filename)
                self._copy_file(lib_path)

    def _copy_zstd_deps(self):
        dst_zstd_deps = os.path.join(self._dst_lib, "common", "zstd_deps.h")
        self._log(f"Copying zstd_deps: {self._zstd_deps} -> {dst_zstd_deps}")
        shutil.copyfile(self._zstd_deps, dst_zstd_deps)

    def _copy_mem(self):
        dst_mem = os.path.join(self._dst_lib, "common", "mem.h")
        self._log(f"Copying mem: {self._mem} -> {dst_mem}")
        shutil.copyfile(self._mem, dst_mem)

    def _hardwire_preprocessor(self, name: str, value: Optional[str] = None, undef=False):
        """
        If value=None then hardwire that it is defined, but not what the value is.
        If undef=True then value must be None.
        If value='' then the macro is defined to '' exactly.
        """
        assert not (undef and value is not None)
        for filepath in self._dst_lib_file_paths():
            file = FileLines(filepath)

    def _hardwire_defines(self):
        self._log("Hardwiring macros")
        partial_preprocessor = PartialPreprocessor(self._defs, self._replaces, self._undefs)
        for filepath in self._dst_lib_file_paths():
            partial_preprocessor.preprocess(filepath)

    def _remove_excludes(self):
        self._log("Removing excluded sections")
        for exclude in self._excludes:
            self._log(f"\tRemoving excluded sections for: {exclude}")
            begin_re = re.compile(f"BEGIN {exclude}")
            end_re = re.compile(f"END {exclude}")
            for filepath in self._dst_lib_file_paths():
                file = FileLines(filepath)
                outlines = []
                skipped = []
                emit = True
                for line in file.lines:
                    if emit and begin_re.search(line) is not None:
                        assert end_re.search(line) is None
                        emit = False
                    if emit:
                        outlines.append(line)
                    else:
                        skipped.append(line)
                        if end_re.search(line) is not None:
                            assert begin_re.search(line) is None
                            self._log(f"\t\tRemoving excluded section: {exclude}")
                            for s in skipped:
                                self._log(f"\t\t\t- {s}")
                            emit = True
                            skipped = []
                if not emit:
                    raise RuntimeError("Excluded section unfinished!")
                file.lines = outlines
                file.write()

    def _rewrite_include(self, original, rewritten):
        self._log(f"\tRewriting include: {original} -> {rewritten}")
        regex = re.compile(f"\\s*#\\s*include\\s*(?P<include>{original})")
        for filepath in self._dst_lib_file_paths():
            file = FileLines(filepath)
            for i, line in enumerate(file.lines):
                match = regex.match(line)
                if match is None:
                    continue
                s = match.start('include')
                e = match.end('include')
                file.lines[i] = line[:s] + rewritten + line[e:]
            file.write()

    def _rewrite_includes(self):
        self._log("Rewriting includes")
        for original, rewritten in self._rewritten_includes:
            self._rewrite_include(original, rewritten)

    def _replace_xxh64_prefix(self):
        if self._xxh64_prefix is None:
            return
        self._log(f"Replacing XXH64 prefix with {self._xxh64_prefix}")
        replacements = []
        if self._xxh64_state is not None:
            replacements.append(
                (re.compile(r"([^\w]|^)(?P<orig>XXH64_state_t)([^\w]|$)"), self._xxh64_state)
            )
        if self._xxh64_prefix is not None:
            replacements.append(
                (re.compile(r"([^\w]|^)(?P<orig>XXH64)[\(_]"), self._xxh64_prefix)
            )
        for filepath in self._dst_lib_file_paths():
            file = FileLines(filepath)
            for i, line in enumerate(file.lines):
                modified = False
                for regex, replacement in replacements:
                    match = regex.search(line)
                    while match is not None:
                        modified = True
                        b = match.start('orig')
                        e = match.end('orig')
                        line = line[:b] + replacement + line[e:]
                        match = regex.search(line)
                if modified:
                    self._log(f"\t- {file.lines[i][:-1]}")
                    self._log(f"\t+ {line[:-1]}")
                file.lines[i] = line
            file.write()

    def _parse_sed(self, sed):
        assert sed[0] == 's'
        delim = sed[1]
        match = re.fullmatch(f's{delim}(.+){delim}(.*){delim}(.*)', sed)
        assert match is not None
        regex = re.compile(match.group(1))
        format_str = match.group(2)
        is_global = match.group(3) == 'g'
        return regex, format_str, is_global

    def _process_sed(self, sed):
        self._log(f"Processing sed: {sed}")
        regex, format_str, is_global = self._parse_sed(sed)

        for filepath in self._dst_lib_file_paths():
            file = FileLines(filepath)
            for i, line in enumerate(file.lines):
                modified = False
                while True:
                    match = regex.search(line)
                    if match is None:
                        break
                    replacement = format_str.format(match.groups(''), match.groupdict(''))
                    b = match.start()
                    e = match.end()
                    line = line[:b] + replacement + line[e:]
                    modified = True
                    if not is_global:
                        break
                if modified:
                    self._log(f"\t- {file.lines[i][:-1]}")
                    self._log(f"\t+ {line[:-1]}")
                file.lines[i] = line
            file.write()

    def _process_seds(self):
        self._log("Processing seds")
        for sed in self._seds:
            self._process_sed(sed)

    def _process_spdx(self):
        if not self._spdx:
            return
        self._log("Processing spdx")
        SPDX_C = "// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause\n"
        SPDX_H_S = "/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */\n"
        for filepath in self._dst_lib_file_paths():
            file = FileLines(filepath)
            if file.lines[0] == SPDX_C or file.lines[0] == SPDX_H_S:
                continue
            for line in file.lines:
                if "SPDX-License-Identifier" in line:
                    raise RuntimeError(f"Unexpected SPDX license identifier: {file.filename} {repr(line)}")
            if file.filename.endswith(".c"):
                file.lines.insert(0, SPDX_C)
            elif file.filename.endswith(".h") or file.filename.endswith(".S"):
                file.lines.insert(0, SPDX_H_S)
            else:
                raise RuntimeError(f"Unexpected file extension: {file.filename}")
            file.write()



    def go(self):
        self._copy_source_lib()
        self._copy_zstd_deps()
        self._copy_mem()
        self._hardwire_defines()
        self._remove_excludes()
        self._rewrite_includes()
        self._replace_xxh64_prefix()
        self._process_seds()
        self._process_spdx()


def parse_optional_pair(defines: [str]) -> [(str, Optional[str])]:
    output = []
    for define in defines:
        parsed = define.split('=')
        if len(parsed) == 1:
            output.append((parsed[0], None))
        elif len(parsed) == 2:
            output.append((parsed[0], parsed[1]))
        else:
            raise RuntimeError(f"Bad define: {define}")
    return output


def parse_pair(rewritten_includes: [str]) -> [(str, str)]:
    output = []
    for rewritten_include in rewritten_includes:
        parsed = rewritten_include.split('=')
        if len(parsed) == 2:
            output.append((parsed[0], parsed[1]))
        else:
            raise RuntimeError(f"Bad rewritten include: {rewritten_include}")
    return output



def main(name, args):
    parser = argparse.ArgumentParser(prog=name)
    parser.add_argument("--zstd-deps", default="zstd_deps.h", help="Zstd dependencies file")
    parser.add_argument("--mem", default="mem.h", help="Memory module")
    parser.add_argument("--source-lib", default="../../lib", help="Location of the zstd library")
    parser.add_argument("--output-lib", default="./freestanding_lib", help="Where to output the freestanding zstd library")
    parser.add_argument("--xxhash", default=None, help="Alternate external xxhash include e.g. --xxhash='<xxhash.h>'. If set xxhash is not included.")
    parser.add_argument("--xxh64-state", default=None, help="Alternate XXH64 state type (excluding _) e.g. --xxh64-state='struct xxh64_state'")
    parser.add_argument("--xxh64-prefix", default=None, help="Alternate XXH64 function prefix (excluding _) e.g. --xxh64-prefix=xxh64")
    parser.add_argument("--rewrite-include", default=[], dest="rewritten_includes", action="append", help="Rewrite an include REGEX=NEW (e.g. '<stddef\\.h>=<linux/types.h>')")
    parser.add_argument("--sed", default=[], dest="seds", action="append", help="Apply a sed replacement. Format: `s/REGEX/FORMAT/[g]`. REGEX is a Python regex. FORMAT is a Python format string formatted by the regex dict.")
    parser.add_argument("--spdx", action="store_true", help="Add SPDX License Identifiers")
    parser.add_argument("-D", "--define", default=[], dest="defs", action="append", help="Pre-define this macro (can be passed multiple times)")
    parser.add_argument("-U", "--undefine", default=[], dest="undefs", action="append", help="Pre-undefine this macro (can be passed multiple times)")
    parser.add_argument("-R", "--replace", default=[], dest="replaces", action="append", help="Pre-define this macro and replace the first ifndef block with its definition")
    parser.add_argument("-E", "--exclude", default=[], dest="excludes", action="append", help="Exclude all lines between 'BEGIN <EXCLUDE>' and 'END <EXCLUDE>'")
    args = parser.parse_args(args)

    # Always remove threading
    if "ZSTD_MULTITHREAD" not in args.undefs:
        args.undefs.append("ZSTD_MULTITHREAD")

    args.defs = parse_optional_pair(args.defs)
    for name, _ in args.defs:
        if name in args.undefs:
            raise RuntimeError(f"{name} is both defined and undefined!")

    # Always set tracing to 0
    if "ZSTD_NO_TRACE" not in (arg[0] for arg in args.defs):
        args.defs.append(("ZSTD_NO_TRACE", None))
        args.defs.append(("ZSTD_TRACE", "0"))

    args.replaces = parse_pair(args.replaces)
    for name, _ in args.replaces:
        if name in args.undefs or name in args.defs:
            raise RuntimeError(f"{name} is both replaced and (un)defined!")

    args.rewritten_includes = parse_pair(args.rewritten_includes)

    external_xxhash = False
    if args.xxhash is not None:
        external_xxhash = True
        args.rewritten_includes.append(('"(\\.\\./common/)?xxhash.h"', args.xxhash))

    if args.xxh64_prefix is not None:
        if not external_xxhash:
            raise RuntimeError("--xxh64-prefix may only be used with --xxhash provided")

    if args.xxh64_state is not None:
        if not external_xxhash:
            raise RuntimeError("--xxh64-state may only be used with --xxhash provided")

    Freestanding(
        args.zstd_deps,
        args.mem,
        args.source_lib,
        args.output_lib,
        external_xxhash,
        args.xxh64_state,
        args.xxh64_prefix,
        args.rewritten_includes,
        args.defs,
        args.replaces,
        args.undefs,
        args.excludes,
        args.seds,
        args.spdx,
    ).go()

if __name__ == "__main__":
    main(sys.argv[0], sys.argv[1:])
