#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import argparse
import os
import site
import subprocess
import sys
from glob import glob

scriptdir = os.path.abspath(os.path.dirname(__file__))
testdir = os.path.join(scriptdir, "t")

site.addsitedir(testdir)
from testlib import Test, equal

parser = argparse.ArgumentParser(description="run hazard analysis tests")
parser.add_argument(
    "--js", default=os.environ.get("JS"), help="JS binary to run the tests with"
)
parser.add_argument(
    "--sixgill",
    default=os.environ.get("SIXGILL", os.path.join(testdir, "sixgill")),
    help="Path to root of sixgill installation",
)
parser.add_argument(
    "--sixgill-bin",
    default=os.environ.get("SIXGILL_BIN"),
    help="Path to sixgill binary dir",
)
parser.add_argument(
    "--sixgill-plugin",
    default=os.environ.get("SIXGILL_PLUGIN"),
    help="Full path to sixgill gcc plugin",
)
parser.add_argument(
    "--gccdir", default=os.environ.get("GCCDIR"), help="Path to GCC installation dir"
)
parser.add_argument("--cc", default=os.environ.get("CC"), help="Path to gcc")
parser.add_argument("--cxx", default=os.environ.get("CXX"), help="Path to g++")
parser.add_argument(
    "--verbose",
    "-v",
    default=0,
    action="count",
    help="Display verbose output, including commands executed",
)
ALL_TESTS = [
    "sixgill-tree",
    "suppression",
    "hazards",
    "exceptions",
    "virtual",
    "graph",
    "types",
]
parser.add_argument(
    "tests",
    nargs="*",
    default=ALL_TESTS,
    help="tests to run",
)

cfg = parser.parse_args()

if not cfg.js:
    sys.exit("Must specify JS binary through environment variable or --js option")
if not cfg.cc:
    if cfg.gccdir:
        cfg.cc = os.path.join(cfg.gccdir, "bin", "gcc")
    else:
        cfg.cc = "gcc"
if not cfg.cxx:
    if cfg.gccdir:
        cfg.cxx = os.path.join(cfg.gccdir, "bin", "g++")
    else:
        cfg.cxx = "g++"
if not cfg.sixgill_bin:
    cfg.sixgill_bin = os.path.join(cfg.sixgill, "usr", "bin")
if not cfg.sixgill_plugin:
    cfg.sixgill_plugin = os.path.join(
        cfg.sixgill, "usr", "libexec", "sixgill", "gcc", "xgill.so"
    )

subprocess.check_call(
    [cfg.js, "-e", 'if (!getBuildConfiguration("has-ctypes")) quit(1)']
)


def binpath(prog):
    return os.path.join(cfg.sixgill_bin, prog)


def make_dir(dirname, exist_ok=True):
    try:
        os.mkdir(dirname)
    except OSError as e:
        if exist_ok and e.strerror == "File exists":
            pass
        else:
            raise


outroot = os.path.join(testdir, "out")
make_dir(outroot)

os.environ["HAZARD_RUN_INTERNAL_TESTS"] = "1"

exclude = []
tests = []
for t in cfg.tests:
    if t.startswith("!"):
        exclude.append(t[1:])
    else:
        tests.append(t)
if len(tests) == 0:
    tests = filter(lambda t: t not in exclude, ALL_TESTS)

failed = set()
passed = set()
for path in tests:
    name = os.path.basename(path)
    indir = os.path.join(testdir, name)
    outdir = os.path.join(outroot, name)
    make_dir(outdir)

    test = Test(indir, outdir, cfg, verbose=cfg.verbose)

    os.chdir(outdir)
    for xdb in glob("*.xdb"):
        os.unlink(xdb)
    print("START TEST {}".format(name), flush=True)
    testpath = os.path.join(indir, "test.py")
    testscript = open(testpath).read()
    testcode = compile(testscript, testpath, "exec")
    try:
        exec(testcode, {"test": test, "equal": equal})
    except subprocess.CalledProcessError:
        print("TEST-FAILED: %s" % name)
        failed.add(name)
    except AssertionError:
        print("TEST-FAILED: %s" % name)
        failed.add(name)
        raise
    else:
        print("TEST-PASSED: %s" % name)
        passed.add(name)

if failed:
    raise Exception("Failed tests: " + " ".join(failed))

print(f"All {len(passed)} tests passed.")
