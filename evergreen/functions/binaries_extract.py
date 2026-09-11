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
import stat
import subprocess
import sys

ZSTD_EXTRACTION = "tar --zstd -xf"


def get_cmd(tarball: str, extraction_command: str) -> list[str]:
    shell = os.environ.get("SHELL", "/bin/bash")
    if sys.platform == "win32":
        proc = subprocess.run(
            ["C:/cygwin/bin/cygpath.exe", "-w", shell], text=True, capture_output=True
        )
        bash = pathlib.Path(proc.stdout.strip())
        return [bash.as_posix(), "-c", f"{extraction_command} {tarball}"]

    return [shell, "-c", f"{extraction_command} {tarball}"]


def clean_directory(target: str, working_dir: str | None) -> None:
    """Remove an extraction destination that an earlier extraction may have left read-only.

    tar applies the archive's directory modes once extraction finishes, so a tree it wrote
    before can come back with unwritable directories. Re-extracting into one fails with
    "Cannot open: File exists": the entry exists and tar cannot unlink it, because removing a
    file needs write permission on its parent directory rather than on the file itself.
    """
    root = pathlib.Path(working_dir).resolve() if working_dir else pathlib.Path.cwd()
    path = (root / target).resolve()
    # A strict descendant, so "." cannot name the extraction directory itself: removing it would
    # delete the cwd the extraction process is about to be started in.
    if root not in path.parents:
        raise ValueError(f"--clean-dir must name a directory strictly inside {root}, got: {target}")
    if not path.exists():
        return

    print(f"Removing existing {path} before extracting.")
    # Restore owner write/search top down, so every directory can be emptied on the way back up.
    for directory, _, _ in os.walk(path):
        current = pathlib.Path(directory)
        if current.is_symlink():
            continue
        current.chmod(stat.S_IMODE(current.stat().st_mode) | stat.S_IRWXU)
    parent = path.parent
    parent.chmod(stat.S_IMODE(parent.stat().st_mode) | stat.S_IRWXU)
    shutil.rmtree(path)


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
parser.add_argument(
    "--clean-dir",
    type=str,
    action="append",
    help="Remove this path, relative to --change-dir, before extracting. Use when the archive "
    "carries read-only directories, which make a repeat extraction fail.",
)
parser.add_argument(
    "--try-zstd",
    type=str,
    action="store",
    help="Try extracting zstd archive first given archive name.",
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

if args.clean_dir:
    for target in args.clean_dir:
        clean_directory(target, working_dir)

# Attempt zstd extraction first, if enabled.
zstd_succeeded = False
if args.try_zstd:
    print("Attempting zstd extraction...")
    zstd_archive = args.try_zstd
    cmd = get_cmd(zstd_archive, ZSTD_EXTRACTION)
    print(f"Extracting: {' '.join(cmd)}")
    proc = subprocess.run(
        cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=working_dir
    )
    zstd_succeeded = proc.returncode == 0

if not zstd_succeeded:
    cmd = get_cmd(tarball, args.extraction_command)
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
