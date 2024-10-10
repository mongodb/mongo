#!/usr/bin/env python3
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
#

import argparse
import glob
import os
import pathlib
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()

parser.add_argument(
    "--change-dir",
    type=str,
    action="store",
    help="The directory to change into to perform the extraction.",
)
parser.add_argument(
    "--extraction-command", type=str, action="store", help="The command to use for the extraction."
)
parser.add_argument(
    "--tarball", type=str, action="store", help="The tarball to perform the extraction on."
)
parser.add_argument(
    "--move-output",
    type=str,
    action="append",
    help="Move an extracted entry to a new location after extraction. Format is colon separated, e.g. '--move-output=file/to/move:path/to/destination'. Can accept glob like wildcards.",
)
parser.add_argument(
    "--optional",
    action="store_true",
    help="Should this fail if extraction fails. Useful for optional success.",
)
args = parser.parse_args()

if args.change_dir:
    working_dir = pathlib.Path(args.change_dir).as_posix()
    tarball = pathlib.Path(args.tarball).resolve().as_posix()
    print(f"Switching to {working_dir} to perform the extraction in.")
    os.makedirs(working_dir, exist_ok=True)
else:
    working_dir = None
    tarball = pathlib.Path(args.tarball).as_posix()

shell = os.environ.get("SHELL", "/bin/bash")

if sys.platform == "win32":
    proc = subprocess.run(
        ["C:/cygwin/bin/cygpath.exe", "-w", shell], text=True, capture_output=True
    )
    bash = pathlib.Path(proc.stdout.strip())
    cmd = [bash.as_posix(), "-c", f"{args.extraction_command} {tarball}"]
else:
    cmd = [shell, "-c", f"{args.extraction_command} {tarball}"]

print(f"Extracting: {' '.join(cmd)}")
proc = subprocess.run(
    cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=working_dir
)

print(proc.stdout)

if args.move_output:
    for arg in args.move_output:
        try:
            src, dst = arg.split(":")
            print(f"Moving {src} to {dst}...")
            files_to_move = glob.glob(src, recursive=True)
            for file in files_to_move:
                result_dst = shutil.move(file, dst)
                print(f"Moved {file} to {result_dst}")
        except ValueError as exc:
            print(f"Bad format, needs to be glob like paths in the from 'src:dst', got: {arg}")
            raise exc

if args.optional:
    sys.exit(0)
sys.exit(proc.returncode)
