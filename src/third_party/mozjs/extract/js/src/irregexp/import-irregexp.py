#!/usr/bin/env python3

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

# This script handles all the mechanical steps of importing irregexp from v8:
#
# 1. Acquire the source: either from github, or optionally from a local copy of v8.
# 2. Copy the contents of v8/src/regexp into js/src/irregexp/imported
#    - Exclude files that we have chosen not to import.
# 3. While doing so, update #includes:
#    - Change "src/regexp/*" to "irregexp/imported/*".
#    - Remove other v8-specific headers completely.
# 4. Add '#include "irregexp/RegExpShim.h" in the necessary places.
# 5. Update the IRREGEXP_VERSION file to include the correct git hash.
#
# Usage:
#  cd path/to/js/src/irregexp
#  ./import-irregexp.py --path path/to/v8/src/regexp
#
# Alternatively, without the --path argument, import-irregexp.py will
# clone v8 from github into a temporary directory.
#
# After running this script, changes to the shim code may be necessary
# to account for changes in upstream irregexp.

import os
import re
import subprocess
import sys
from pathlib import Path


def copy_and_update_includes(src_path, dst_path):
    # List of header files that need to include the shim header
    need_shim = [
        "property-sequences.h",
        "regexp-ast.h",
        "regexp-bytecode-peephole.h",
        "regexp-bytecodes.h",
        "regexp-dotprinter.h",
        "regexp-error.h",
        "regexp.h",
        "regexp-macro-assembler.h",
        "regexp-parser.h",
        "regexp-stack.h",
        "special-case.h",
    ]

    src = open(str(src_path), "r")
    dst = open(str(dst_path), "w")

    # 1. Rewrite includes of V8 regexp headers:
    #    Note that we exclude regexp-flags.h and provide our own definition.
    regexp_include = re.compile('#include "src/regexp(?!/regexp-flags.h)')
    regexp_include_new = '#include "irregexp/imported'

    # 2. Remove includes of other V8 headers
    other_include = re.compile('#include "src/')

    # 3. If needed, add '#include "irregexp/RegExpShim.h"'.
    #    Note: We get a little fancy to ensure that header files are
    #    in alphabetic order. `need_to_add_shim` is true if we still
    #    have to add the shim header in this file. `adding_shim_now`
    #    is true if we have found a '#include "src/*' and we are just
    #    waiting to find an empty line so that we can insert the shim
    #    header in the right place.
    need_to_add_shim = src_path.name in need_shim
    adding_shim_now = False

    for line in src:
        if adding_shim_now:
            if line == "\n":
                dst.write('#include "irregexp/RegExpShim.h"\n')
                need_to_add_shim = False
                adding_shim_now = False

        if regexp_include.search(line):
            dst.write(re.sub(regexp_include, regexp_include_new, line))
        elif other_include.search(line):
            if need_to_add_shim:
                adding_shim_now = True
        else:
            dst.write(line)


def import_from(srcdir, dstdir):
    excluded = [
        "DIR_METADATA",
        "OWNERS",
        "regexp.cc",
        "regexp-flags.h",
        "regexp-utils.cc",
        "regexp-utils.h",
        "regexp-macro-assembler-arch.h",
    ]

    for file in srcdir.iterdir():
        if file.is_dir():
            continue
        if str(file.name) in excluded:
            continue
        copy_and_update_includes(file, dstdir / "imported" / file.name)


if __name__ == "__main__":
    import argparse
    import tempfile

    # This script should be run from js/src/irregexp to work correctly.
    current_path = Path(os.getcwd())
    expected_path = "js/src/irregexp"
    if not current_path.match(expected_path):
        raise RuntimeError("%s must be run from %s" % (sys.argv[0], expected_path))

    parser = argparse.ArgumentParser(description="Import irregexp from v8")
    parser.add_argument("-p", "--path", help="path to v8/src/regexp", required=False)
    args = parser.parse_args()

    if args.path:
        src_path = Path(args.path)
        provided_path = "the command-line"
    elif "TASK_ID" in os.environ:
        src_path = Path("/builds/worker/v8/")
        subprocess.run("git pull origin master", shell=True, cwd=src_path)

        src_path = Path("/builds/worker/v8/src/regexp")
        provided_path = "the hardcoded path in the taskcluster image"
    elif "V8_GIT" in os.environ:
        src_path = Path(os.environ["V8_GIT"])
        provided_path = "the V8_GIT environment variable"
    else:
        tempdir = tempfile.TemporaryDirectory()
        v8_git = "https://github.com/v8/v8.git"
        clone = "git clone --depth 1 %s %s" % (v8_git, tempdir.name)
        os.system(clone)
        src_path = Path(tempdir.name) / "src/regexp"
        provided_path = "the temporary git checkout"

    if not (src_path / "regexp.h").exists():
        print("Could not find regexp.h in the path provided from", provided_path)
        print("Usage:\n  import-irregexp.py [--path <path/to/v8/src/regexp>]")
        sys.exit(1)

    if "MACH_VENDOR" not in os.environ:
        print(
            "Running this script outside ./mach vendor is not recommended - ",
            "You will need to update moz.yaml manually",
        )
        print("We recommend instead `./mach vendor js/src/irregexp/moz.yaml`")
        response = input("Type Y to continue... ")
        if response.lower() != "y":
            sys.exit(1)

    import_from(src_path, current_path)
