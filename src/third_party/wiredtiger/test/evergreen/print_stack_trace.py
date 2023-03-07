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

    def dump(self, exe_path: str, core_path: str, dump_all: bool, output_file: str):
        """Dump stack trace."""
        if self.dbg is None:
            sys.exit("Debugger lldb not found,"
                     "skipping dumping of {}".format(core_path))

        cmds = []
        if dump_all:
            cmds.append("thread apply all backtrace -c 30")
        else:
            cmds.append("backtrace -c 30")
        cmds.append("quit")

        output = None
        if (output_file):
            try:
                output = open(output_file, "w")
            except OSError as e :
                raise e
        subprocess.run([self.dbg, "--batch"] + [exe_path, "-c", core_path] +
                       list(itertools.chain.from_iterable([['-o', b] for b in cmds])),
                       check=True, stdout=output)


class GDBDumper:
    """GDBDumper class - prints stack traces on Linux"""
    def __init__(self):
        self.dbg = self._find_debugger("gdb")

    @staticmethod
    def _find_debugger(debugger: str):
        """Find the installed debugger."""
        return which(debugger)

    def dump(self, exe_path: str, core_path: str, lib_path: str, dump_all: bool, output_file: str):
        """Dump stack trace."""
        if self.dbg is None:
            sys.exit("Debugger gdb not found,"
                     "skipping dumping of {}".format(core_path))

        cmds = []
        if lib_path:
            cmds.append("set solib-search-path " + lib_path)

        if dump_all:
            cmds.append("thread apply all backtrace 30")
        else:
            cmds.append("backtrace 30")
        cmds.append("quit")

        output = None
        if (output_file):
            try:
                output = open(output_file, "w")
            except OSError as e :
                raise e
        subprocess.run([self.dbg, "--batch", "--quiet"] +
                       list(itertools.chain.from_iterable([['-ex', b] for b in cmds])) +
                       [exe_path, core_path],
                       check=True, stdout=output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--core_path',
                        help='directory path to the core dumps')
    parser.add_argument('-l', '--lib_path', help='library path')
    args = parser.parse_args()

    # If the lib_path is not provided then search the current dir.
    lib_path = "." if args.lib_path is None else args.lib_path

    # If the core_path is not provided then search the current dir.
    core_path = "." if args.core_path is None else args.core_path

    # Append the path of the core files present in the core path in a list.
    core_files = []
    regex = re.compile(r'dump.*core', re.IGNORECASE)
    for root, _, files in os.walk(core_path):
        for file in files:
            if regex.match(file):
                core_files.append(os.path.join(root, file))

    for core_file_path in core_files:
        print(border_msg(core_file_path), flush=True)

        # Get the executable from the core file itself.
        proc = subprocess.Popen(["file", core_file_path], stdout=subprocess.PIPE)
        # The file command prints something similar to:
        # WT_TEST/test_practice.0/dump_python3.16562.core: ELF 64-bit LSB core file x86-64, version 1 (SYSV),
        # SVR4-style, from 'python3 /home/ubuntu/wiredtiger/test/suite/run.py -j 1 -p test_practice',
        # real uid: 1000, effective uid: 1000, real gid: 1000, effective gid: 1000, execfn: '/usr/bin/python3', platform: 'x86_64'
        output = str(proc.communicate())

        # The field of interest is execfn: '/usr/bin/python3' from the example above.
        start = output.find('execfn: ')
        if start < 0:
            print("The 'execfn' is missing, skipping core...")
            continue
        # Fetch the value of execfn. This is the executable path!
        executable_path = re.search(r'\'.*?\'', output[start:]).group(0)
        executable_path = executable_path.replace("'", "")

        # If the core dump comes from a Python test, we don't need to construct the executable path.
        if "python" not in executable_path.lower():
            # We may have the executable path already. If not, we need to find it.
            if not os.access(executable_path, os.X_OK):
                # The executable is where the core dump is.
                dirname = core_file_path.rsplit('/', 1)[0]
                executable_path = dirname + '/' + executable_path

        if sys.platform.startswith('linux'):
            dbg = GDBDumper()
            dbg.dump(executable_path, core_file_path, lib_path, False, None)

            # Extract the filename from the core file path, to create a stacktrace output file.
            file_name, _ = os.path.splitext(os.path.basename(core_file_path))
            dbg.dump(executable_path, core_file_path, lib_path, True, file_name + ".stacktrace.txt")
        elif sys.platform.startswith('darwin'):
            # FIXME - macOS to be supported in WT-8976
            # dbg = LLDBDumper()
            # dbg.dump(args.executable_path, core_file_path, False, None)

            # Extract the filename from the core file path, to create a stacktrace output file.
            # file_name, _ = os.path.splitext(os.path.basename(core_file_path))
            # dbg.dump(executable_path, core_file_path, True, file_name + ".stacktrace.txt")
            pass
        elif sys.platform.startswith('win32') or sys.platform.startswith('cygwin'):
            # FIXME - Windows to be supported in WT-8937
            pass

if __name__ == "__main__":
    main()
