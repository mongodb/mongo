#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import site
import subprocess
import argparse

testdir = os.path.abspath(os.path.join(os.path.dirname(__file__), 't'))
site.addsitedir(testdir)
from testlib import Test, equal

scriptdir = os.path.abspath(os.path.dirname(__file__))

parser = argparse.ArgumentParser(description='run hazard analysis tests')
parser.add_argument(
    '--js', default=os.environ.get('JS'),
    help='JS binary to run the tests with')
parser.add_argument(
    '--sixgill', default=os.environ.get('SIXGILL', os.path.join(testdir, "sixgill")),
    help='Path to root of sixgill installation')
parser.add_argument(
    '--sixgill-bin', default=os.environ.get('SIXGILL_BIN'),
    help='Path to sixgill binary dir')
parser.add_argument(
    '--sixgill-plugin', default=os.environ.get('SIXGILL_PLUGIN'),
    help='Full path to sixgill gcc plugin')
parser.add_argument(
    '--gccdir', default=os.environ.get('GCCDIR'),
    help='Path to GCC installation dir')
parser.add_argument(
    '--cc', default=os.environ.get('CC'),
    help='Path to gcc')
parser.add_argument(
    '--cxx', default=os.environ.get('CXX'),
    help='Path to g++')
parser.add_argument(
    '--verbose', '-v', action='store_true',
    help='Display verbose output, including commands executed')
parser.add_argument(
    'tests', nargs='*', default=['sixgill-tree', 'suppression', 'hazards', 'exceptions'],
    help='tests to run')

cfg = parser.parse_args()

if not cfg.js:
    exit('Must specify JS binary through environment variable or --js option')
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
    cfg.sixgill_plugin = os.path.join(cfg.sixgill, "usr", "libexec", "sixgill", "gcc", "xgill.so")

subprocess.check_call([cfg.js, '-e', 'if (!getBuildConfiguration()["has-ctypes"]) quit(1)'])

def binpath(prog):
    return os.path.join(cfg.sixgill_bin, prog)

try:
    os.mkdir(os.path.join('t', 'out'))
except OSError:
    pass

for name in cfg.tests:
    name = os.path.basename(name)
    indir = os.path.join(testdir, name)
    outdir = os.path.join(testdir, 'out', name)
    try:
        os.mkdir(outdir)
    except OSError:
        pass

    test = Test(indir, outdir, cfg, verbose=cfg.verbose)

    os.chdir(outdir)
    subprocess.call(["sh", "-c", "rm *.xdb"])
    if cfg.verbose:
        print("Running test %s" % name)
    testpath = os.path.join(indir, "test.py")
    testscript = open(testpath).read()
    testcode = compile(testscript, testpath, 'exec')
    exec(testcode, {'test': test, 'equal': equal})
    print("TEST-PASSED: %s" % name)
