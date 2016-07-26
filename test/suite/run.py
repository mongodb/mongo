#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
#
# run.py
#      Command line test runner
#

import glob, json, os, re, sys

# Set paths
suitedir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(suitedir))
wt_3rdpartydir = os.path.join(wt_disttop, 'test', '3rdparty')

# Check for a local build that contains the wt utility. First check in
# current working directory, then in build_posix and finally in the disttop
# directory. This isn't ideal - if a user has multiple builds in a tree we
# could pick the wrong one.
if os.path.isfile(os.path.join(os.getcwd(), 'wt')):
    wt_builddir = os.getcwd()
elif os.path.isfile(os.path.join(wt_disttop, 'wt')):
    wt_builddir = wt_disttop
elif os.path.isfile(os.path.join(wt_disttop, 'build_posix', 'wt')):
    wt_builddir = os.path.join(wt_disttop, 'build_posix')
elif os.path.isfile(os.path.join(wt_disttop, 'wt.exe')):
    wt_builddir = wt_disttop
else:
    print 'Unable to find useable WiredTiger build'
    sys.exit(1)

# Cannot import wiredtiger and supporting utils until we set up paths
# We want our local tree in front of any installed versions of WiredTiger.
# Don't change sys.path[0], it's the dir containing the invoked python script.
sys.path.insert(1, os.path.join(wt_builddir, 'lang', 'python'))
sys.path.insert(1, os.path.join(wt_disttop, 'lang', 'python'))

# Add all 3rd party directories: some have code in subdirectories
for d in os.listdir(wt_3rdpartydir):
    for subdir in ('lib', 'python', ''):
        if os.path.exists(os.path.join(wt_3rdpartydir, d, subdir)):
            sys.path.insert(1, os.path.join(wt_3rdpartydir, d, subdir))
            break

import wttest
# Use the same version of unittest found by wttest.py
unittest = wttest.unittest
from testscenarios.scenarios import generate_scenarios

def usage():
    print 'Usage:\n\
  $ cd build_posix\n\
  $ python ../test/suite/run.py [ options ] [ tests ]\n\
\n\
Options:\n\
  -C file | --configcreate file  create a config file for controlling tests\n\
  -c file | --config file        use a config file for controlling tests\n\
  -D dir  | --dir dir            use dir rather than WT_TEST.\n\
                                 dir is removed/recreated as a first step.\n\
  -d      | --debug              run with \'pdb\', the python debugger\n\
  -g      | --gdb                all subprocesses (like calls to wt) use gdb\n\
  -h      | --help               show this message\n\
  -j N    | --parallel N         run all tests in parallel using N processes\n\
  -l      | --long               run the entire test suite\n\
  -p      | --preserve           preserve output files in WT_TEST/<testname>\n\
  -t      | --timestamp          name WT_TEST according to timestamp\n\
  -v N    | --verbose N          set verboseness to N (0<=N<=3, default=1)\n\
\n\
Tests:\n\
  may be a file name in test/suite: (e.g. test_base01.py)\n\
  may be a subsuite name (e.g. \'base\' runs test_base*.py)\n\
\n\
  When -C or -c are present, there may not be any tests named.\n\
'

# capture the category (AKA 'subsuite') part of a test name,
# e.g. test_util03 -> util
reCatname = re.compile(r"test_([^0-9]+)[0-9]*")

def addScenarioTests(tests, loader, testname):
    loaded = loader.loadTestsFromName(testname)
    tests.addTests(generate_scenarios(loaded))

def configRecord(cmap, tup):
    """
    Records this tuple in the config.  It is marked as None
    (appearing as null in json), so it can be easily adjusted
    in the output file.
    """
    tuplen = len(tup)
    pos = 0
    for name in tup:
        last = (pos == tuplen - 1)
        pos += 1
        if not name in cmap:
            if last:
                cmap[name] = {"run":None}
            else:
                cmap[name] = {"run":None, "sub":{}}
        if not last:
            cmap = cmap[name]["sub"]

def configGet(cmap, tup):
    """
    Answers the question, should we do this test, given this config file?
    Following the values of the tuple through the map,
    returning the first non-null value.  If all values are null,
    return True (handles tests that may have been added after the
    config was generated).
    """
    for name in tup:
        if not name in cmap:
            return True
        run = cmap[name]["run"] if "run" in cmap[name] else None
        if run != None:
            return run
        cmap = cmap[name]["sub"] if "sub" in cmap[name] else {}
    return True

def configApplyInner(suites, configmap, configwrite):
    newsuite = unittest.TestSuite()
    for s in suites:
        if type(s) is unittest.TestSuite:
            newsuite.addTest(configApplyInner(s, configmap, configwrite))
        else:
            modname = s.__module__
            catname = re.sub(reCatname, r"\1", modname)
            classname = s.__class__.__name__
            methname = s._testMethodName

            tup = (catname, modname, classname, methname)
            add = True
            if configwrite:
                configRecord(configmap, tup)
            else:
                add = configGet(configmap, tup)
            if add:
                newsuite.addTest(s)
    return newsuite

def configApply(suites, configfilename, configwrite):
    configmap = None
    if not configwrite:
        with open(configfilename, 'r') as f:
            line = f.readline()
            while line != '\n' and line != '':
                line = f.readline()
            configmap = json.load(f)
    else:
        configmap = {}
    newsuite = configApplyInner(suites, configmap, configwrite)
    if configwrite:
        with open(configfilename, 'w') as f:
            f.write("""# Configuration file for wiredtiger test/suite/run.py,
# generated with '-C filename' and consumed with '-c filename'.
# This shows the hierarchy of tests, and can be used to rerun with
# a specific subset of tests.  The value of "run" controls whether
# a test or subtests will be run:
#
#   true   turn on a test and all subtests (overriding values beneath)
#   false  turn on a test and all subtests (overriding values beneath)
#   null   do not effect subtests
#
# If a test does not appear, or is marked as '"run": null' all the way down,
# then the test is run.
#
# The remainder of the file is in JSON format.
# !!! There must be a single blank line following this line!!!

""")
            json.dump(configmap, f, sort_keys=True, indent=4)
    return newsuite

def testsFromArg(tests, loader, arg):
    # If a group of test is mentioned, do all tests in that group
    # e.g. 'run.py base'
    groupedfiles = glob.glob(suitedir + os.sep + 'test_' + arg + '*.py')
    if len(groupedfiles) > 0:
        for file in groupedfiles:
            testsFromArg(tests, loader, os.path.basename(file))
        return

    # Explicit test class names
    if not arg[0].isdigit():
        if arg.endswith('.py'):
            arg = arg[:-3]
        addScenarioTests(tests, loader, arg)
        return

    # Deal with ranges
    if '-' in arg:
        start, end = (int(a) for a in arg.split('-'))
    else:
        start, end = int(arg), int(arg)
    for t in xrange(start, end+1):
        addScenarioTests(tests, loader, 'test%03d' % t)

if __name__ == '__main__':
    tests = unittest.TestSuite()

    # Turn numbers and ranges into test module names
    preserve = timestamp = debug = gdbSub = longtest = False
    parallel = 0
    configfile = None
    configwrite = False
    dirarg = None
    verbose = 1
    args = sys.argv[1:]
    testargs = []
    while len(args) > 0:
        arg = args.pop(0)
        from unittest import defaultTestLoader as loader

        # Command line options
        if arg[0] == '-':
            option = arg[1:]
            if option == '-dir' or option == 'D':
                if dirarg != None or len(args) == 0:
                    usage()
                    sys.exit(2)
                dirarg = args.pop(0)
                continue
            if option == '-debug' or option == 'd':
                debug = True
                continue
            if option == '-gdb' or option == 'g':
                gdbSub = True
                continue
            if option == '-help' or option == 'h':
                usage()
                sys.exit(0)
            if option == '-long' or option == 'l':
                longtest = True
                continue
            if option == '-parallel' or option == 'j':
                if parallel != 0 or len(args) == 0:
                    usage()
                    sys.exit(2)
                parallel = int(args.pop(0))
                continue
            if option == '-preserve' or option == 'p':
                preserve = True
                continue
            if option == '-timestamp' or option == 't':
                timestamp = True
                continue
            if option == '-verbose' or option == 'v':
                if len(args) == 0:
                    usage()
                    sys.exit(2)
                verbose = int(args.pop(0))
                if verbose > 3:
                        verbose = 3
                if verbose < 0:
                        verbose = 0
                continue
            if option == '-config' or option == 'c':
                if configfile != None or len(args) == 0:
                    usage()
                    sys.exit(2)
                configfile = args.pop(0)
                continue
            if option == '-configcreate' or option == 'C':
                if configfile != None or len(args) == 0:
                    usage()
                    sys.exit(2)
                configfile = args.pop(0)
                configwrite = True
                continue
            print 'unknown arg: ' + arg
            usage()
            sys.exit(2)
        testargs.append(arg)

    # All global variables should be set before any test classes are loaded.
    # That way, verbose printing can be done at the class definition level.
    wttest.WiredTigerTestCase.globalSetup(preserve, timestamp, gdbSub,
                                          verbose, dirarg, longtest)

    # Without any tests listed as arguments, do discovery
    if len(testargs) == 0:
        from discover import defaultTestLoader as loader
        suites = loader.discover(suitedir)
        suites = sorted(suites, key=lambda c: str(list(c)[0]))
        if configfile != None:
            suites = configApply(suites, configfile, configwrite)
        tests.addTests(generate_scenarios(suites))
    else:
        for arg in testargs:
            testsFromArg(tests, loader, arg)

    if debug:
        import pdb
        pdb.set_trace()

    result = wttest.runsuite(tests, parallel)
    sys.exit(0 if result.wasSuccessful() else 1)
