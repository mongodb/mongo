#!/usr/bin/env python3

# ################################################################
# Copyright (c) Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ################################################################

import enum
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")

RELDIRS = [
    "doc",
    "examples",
    "lib",
    "programs",
    "tests",
    "contrib/linux-kernel",
]

REL_EXCLUDES = [
    "contrib/linux-kernel/test/include",
]

def to_abs(d):
    return os.path.normpath(os.path.join(ROOT, d)) + "/"

DIRS = [to_abs(d) for d in RELDIRS]
EXCLUDES = [to_abs(d) for d in REL_EXCLUDES]

SUFFIXES = [
    ".c",
    ".h",
    "Makefile",
    ".mk",
    ".py",
    ".S",
]

# License should certainly be in the first 10 KB.
MAX_BYTES = 10000
MAX_LINES = 50

LICENSE_LINES = [
    "This source code is licensed under both the BSD-style license (found in the",
    "LICENSE file in the root directory of this source tree) and the GPLv2 (found",
    "in the COPYING file in the root directory of this source tree).",
    "You may select, at your option, one of the above-listed licenses.",
]

COPYRIGHT_EXCEPTIONS = {
    # From zstdmt
    "threading.c",
    "threading.h",
    # From divsufsort
    "divsufsort.c",
    "divsufsort.h",
}

LICENSE_EXCEPTIONS = {
    # From divsufsort
    "divsufsort.c",
    "divsufsort.h",
    # License is slightly different because it references GitHub
    "linux_zstd.h",
}


def valid_copyright(lines):
    YEAR_REGEX = re.compile("\d\d\d\d|present")
    for line in lines:
        line = line.strip()
        if "Copyright" not in line:
            continue
        if "present" in line:
            return (False, f"Copyright line '{line}' contains 'present'!")
        if "Facebook, Inc" not in line:
            return (False, f"Copyright line '{line}' does not contain 'Facebook, Inc'")
        year = YEAR_REGEX.search(line)
        if year is not None:
            return (False, f"Copyright line '{line}' contains {year.group(0)}; it should be yearless")
        if " (c) " not in line:
            return (False, f"Copyright line '{line}' does not contain ' (c) '!")
        return (True, "")
    return (False, "Copyright not found!")


def valid_license(lines):
    for b in range(len(lines)):
        if LICENSE_LINES[0] not in lines[b]:
            continue
        for l in range(len(LICENSE_LINES)):
            if LICENSE_LINES[l] not in lines[b + l]:
                message = f"""Invalid license line found starting on line {b + l}!
Expected: '{LICENSE_LINES[l]}'
Actual: '{lines[b + l]}'"""
                return (False, message)
        return (True, "")
    return (False, "License not found!")


def valid_file(filename):
    with open(filename, "r") as f:
        lines = f.readlines(MAX_BYTES)
    lines = lines[:min(len(lines), MAX_LINES)]

    ok = True
    if os.path.basename(filename) not in COPYRIGHT_EXCEPTIONS:
        c_ok, c_msg = valid_copyright(lines)
        if not c_ok:
            print(f"{filename}: {c_msg}", file=sys.stderr)
            ok = False
    if os.path.basename(filename) not in LICENSE_EXCEPTIONS:
        l_ok, l_msg = valid_license(lines)
        if not l_ok:
            print(f"{filename}: {l_msg}", file=sys.stderr)
            ok = False
    return ok


def exclude(filename):
    for x in EXCLUDES:
        if filename.startswith(x):
            return True
    return False

def main():
    invalid_files = []
    for directory in DIRS:
        for suffix in SUFFIXES:
            files = set(glob.glob(f"{directory}/**/*{suffix}", recursive=True))
            for filename in files:
                if exclude(filename):
                    continue
                if not valid_file(filename):
                    invalid_files.append(filename)
    if len(invalid_files) > 0:
        print("Fail!", file=sys.stderr)
        for f in invalid_files:
            print(f)
        return 1
    else:
        print("Pass!", file=sys.stderr)
        return 0

if __name__ == "__main__":
    sys.exit(main())
