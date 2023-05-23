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
import subprocess
import os
import sys
import pathlib

parser = argparse.ArgumentParser()

parser.add_argument('--change-dir', type=str, action='store',
                    help="The directory to change into to perform the extraction.")
parser.add_argument('--extraction-command', type=str, action='store',
                    help="The command to use for the extraction.")
parser.add_argument('--tarball', type=str, action='store',
                    help="The tarball to perform the extraction on.")

args = parser.parse_args()

if args.change_dir:
    working_dir = pathlib.Path(args.change_dir).as_posix()
    tarball = pathlib.Path(args.tarball).resolve().as_posix()
    print(f"Switching to {working_dir} to perform the extraction in.")
    os.makedirs(working_dir, exist_ok=True)
else:
    working_dir = None
    tarball = pathlib.Path(args.tarball).as_posix()

if sys.platform == 'win32':
    proc = subprocess.run(['C:/cygwin/bin/cygpath.exe', '-w', os.environ['SHELL']], text=True,
                          capture_output=True)
    bash = pathlib.Path(proc.stdout.strip())
    cmd = [bash.as_posix(), '-c', f"{args.extraction_command} {tarball}"]
else:
    cmd = [os.environ['SHELL'], '-c', f"{args.extraction_command} {tarball}"]

print(f"Extracting: {' '.join(cmd)}")
proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                      cwd=working_dir)

print(proc.stdout)
sys.exit(proc.returncode)
