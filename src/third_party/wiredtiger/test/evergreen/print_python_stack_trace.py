#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import argparse
import itertools
import os
import re
import subprocess
import sys
from shutil import which


def border_msg(msg: str):
    count = len(msg) + 2
    dash = "-" * count
    return "+{dash}+\n| {msg} |\n+{dash}+".format(dash=dash, msg=msg)


class LLDBDumper:
    """LLDBDumper class - prints stack traces on macOS"""
    def __init__(self):
        self.dbg = self._find_debugger("lldb")

    @staticmethod
    def _find_debugger(debugger: str):
        """Find the installed debugger."""
        return which(debugger)

    def dump(self, exe_path: str, core_path: str):
        """Dump stack trace."""
        if self.dbg is None:
            sys.exit("Debugger lldb not found,"
                     "skipping dumping of {}".format(core_path))

        cmds = [
            "thread backtrace all -c 30",
            "quit"
        ]

        subprocess.run([self.dbg, "--batch"] + [exe_path, "-c", core_path] +
                       list(itertools.chain.from_iterable([['-o', b] for b in cmds])),
                       check=True)


class GDBDumper:
    """GDBDumper class - prints stack traces on Linux"""
    def __init__(self):
        self.dbg = self._find_debugger("gdb")

    @staticmethod
    def _find_debugger(debugger: str):
        """Find the installed debugger."""
        return which(debugger)

    def dump(self, exe_path: str, core_path: str, lib_path: str):
        """Dump stack trace."""
        if self.dbg is None:
            sys.exit("Debugger gdb not found,"
                     "skipping dumping of {}".format(core_path))

        cmds = []
        if lib_path:
            cmds.append("set solib-search-path " + lib_path)
        cmds.extend([
            "thread apply all backtrace 30",
            "quit"
        ])

        subprocess.run([self.dbg, "--batch", "--quiet"] +
                       list(itertools.chain.from_iterable([['-ex', b] for b in cmds])) +
                       [exe_path, core_path],
                       check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-e', '--executable_path',
                        help='path to the executable',
                        required=True)
    parser.add_argument('-c', '--core_path',
                        help='directory path to the core dumps',
                        required=True)
    parser.add_argument('-l', '--lib_path', help='library path')
    args = parser.parse_args()

    # Store the path of the core files as a list.
    core_files = []
    regex = re.compile(r'.*dump.*python.*', re.IGNORECASE)
    for root, _, files in os.walk(args.core_path):
        for file in files:
            if regex.match(file):
                core_files.extend([os.path.join(root, file)])

    for core_file_path in core_files:
        print(border_msg(core_file_path), flush=True)
        if sys.platform.startswith('linux'):
            dbg = GDBDumper()
            dbg.dump(args.executable_path, core_file_path, args.lib_path)
        elif sys.platform.startswith('darwin'):
            # FIXME - macOS to be supported in WT-8976
            # dbg = LLDBDumper()
            # dbg.dump(args.executable_path, core_file_path)
            pass
        elif sys.platform.startswith('win32') or sys.platform.startswith('cygwin'):
            # FIXME - Windows to be supported in WT-8937
            pass

if __name__ == "__main__":
    main()
