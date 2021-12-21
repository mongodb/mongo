#!/usr/bin/env python
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
#
# [TEST_TAGS]
# ignored_file
# [END_TAGS]
#
# run.py
#      Command line test runner
#

from __future__ import print_function
import glob, json, os, random, re, sys

if sys.version_info[0] <= 2:
    print('WiredTiger requires Python version 3.0 or above')
    sys.exit(1)

# Set paths
suitedir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(suitedir))
wt_3rdpartydir = os.path.join(wt_disttop, 'test', '3rdparty')

# Check for a local build that contains the wt utility. First check in
# current working directory, then in build_posix and finally in the disttop
# directory. This isn't ideal - if a user has multiple builds in a tree we
# could pick the wrong one. We also need to account for the fact that there
# may be an executable 'wt' file the build directory and a subordinate .libs
# directory.
env_builddir = os.getenv('WT_BUILDDIR')
curdir = os.getcwd()
if env_builddir and os.path.isfile(os.path.join(env_builddir, 'wt')):
    wt_builddir = env_builddir
elif os.path.basename(curdir) == '.libs' and \
   os.path.isfile(os.path.join(curdir, os.pardir, 'wt')):
    wt_builddir = os.path.join(curdir, os.pardir)
elif os.path.isfile(os.path.join(curdir, 'wt')):
    wt_builddir = curdir
elif os.path.isfile(os.path.join(curdir, 'wt.exe')):
    wt_builddir = curdir
elif os.path.isfile(os.path.join(wt_disttop, 'wt')):
    wt_builddir = wt_disttop
elif os.path.isfile(os.path.join(wt_disttop, 'build_posix', 'wt')):
    wt_builddir = os.path.join(wt_disttop, 'build_posix')
elif os.path.isfile(os.path.join(wt_disttop, 'wt.exe')):
    wt_builddir = wt_disttop
else:
    print('Unable to find useable WiredTiger build')
    sys.exit(1)

# Cannot import wiredtiger and supporting utils until we set up paths
# We want our local tree in front of any installed versions of WiredTiger.
# Don't change sys.path[0], it's the dir containing the invoked python script.

sys.path.insert(1, os.path.join(wt_builddir, 'lang', 'python'))

# Append to a colon separated path in the environment
def append_env_path(name, value):
    path = os.environ.get(name)
    if path == None:
        v = value
    else:
        v = path + ':' + value
    os.environ[name] = v

# If we built with libtool, explicitly put its install directory in our library
# search path. This only affects library loading for subprocesses, like 'wt'.
libsdir = os.path.join(wt_builddir, '.libs')
if os.path.isdir(libsdir):
    append_env_path('LD_LIBRARY_PATH', libsdir)
    if sys.platform == "darwin":
        append_env_path('DYLD_LIBRARY_PATH', libsdir)

# Add all 3rd party directories: some have code in subdirectories
for d in os.listdir(wt_3rdpartydir):
    for subdir in ('lib', 'python', ''):
        if os.path.exists(os.path.join(wt_3rdpartydir, d, subdir)):
            sys.path.insert(1, os.path.join(wt_3rdpartydir, d, subdir))
            break

# unittest will be imported later, near to when it is needed.
unittest = None

def usage():
    print('Usage:\n\
  $ cd build_posix\n\
  $ python ../test/suite/run.py [ options ] [ tests ]\n\
\n\
Options:\n\
            --asan               run with an ASAN enabled shared library\n\
  -b K/N  | --batch K/N          run batch K of N, 0 <= K < N. The tests\n\
                                 are split into N batches and the Kth is run.\n\
  -C file | --configcreate file  create a config file for controlling tests\n\
  -c file | --config file        use a config file for controlling tests\n\
  -D dir  | --dir dir            use dir rather than WT_TEST.\n\
                                 dir is removed/recreated as a first step.\n\
  -d      | --debug              run with \'pdb\', the python debugger\n\
  -n      | --dry-run            perform a dry-run, listing all scenarios to\n\
                                 be run without executing any.\n\
  -g      | --gdb                all subprocesses (like calls to wt) use gdb\n\
  -h      | --help               show this message\n\
          | --hook name[=arg]    set up hooks from hook_<name>.py, with optional arg\n\
  -j N    | --parallel N         run all tests in parallel using N processes\n\
  -l      | --long               run the entire test suite\n\
          | --noremove           do not remove WT_TEST or -D target before run\n\
  -p      | --preserve           preserve output files in WT_TEST/<testname>\n\
  -r N    | --random-sample N    randomly sort scenarios to be run, then\n\
                                 execute every Nth (2<=N<=1000) scenario.\n\
  -s N    | --scenario N         use scenario N (N can be symbolic, number, or\n\
                                 list of numbers and ranges in the form 1,3-5,7)\n\
  -t      | --timestamp          name WT_TEST according to timestamp\n\
  -v N    | --verbose N          set verboseness to N (0<=N<=3, default=1)\n\
  -i      | --ignore-stdout      dont fail on unexpected stdout or stderr\n\
  -R      | --randomseed         run with random seeds for generates random numbers\n\
  -S      | --seed               run with two seeds that generates random numbers, \n\
                                 format "seed1.seed2", seed1 or seed2 can\'t be zero\n\
  -z      | --zstd               run the zstd tests\n\
\n\
Tests:\n\
  may be a file name in test/suite: (e.g. test_base01.py)\n\
  may be a subsuite name (e.g. \'base\' runs test_base*.py)\n\
\n\
  When -C or -c are present, there may not be any tests named.\n\
  When -s is present, there must be a test named.\n\
')

# Find an executable of the given name in the execution path.
def which(name):
    path = os.getenv('PATH')
    for pathdir in path.split(os.path.pathsep):
        fname = os.path.join(pathdir, name)
        if os.path.exists(fname) and os.access(fname, os.X_OK):
            return fname
    return None

# Follow a symbolic link, returning the target
def follow_symlinks(pathname):
    return os.path.realpath(pathname)

# Find all instances of a filename under a directory
def find(topdir, filename):
    results = []
    for root, dirs, files in os.walk(topdir, followlinks=True):
        if filename in files:
            results.append(os.path.join(root, filename))
    return results

# Show an environment variable if verbose enough.
def show_env(verbose, envvar):
    if verbose >= 2:
        print(envvar + "=" + os.getenv(envvar))

# capture the category (AKA 'subsuite') part of a test name,
# e.g. test_util03 -> util
reCatname = re.compile(r"test_([^0-9]+)[0-9]*")

# Look for a list of the form 0-9,11,15-17.
def parse_int_list(str):
    # Use a dictionary as the result set to avoid repeated list scans.
    # (Only the keys are used; the values are ignored.)
    ret = {}
    # Divide the input into ranges separated by commas.
    for r in str.split(","):
        # Split the range we got (if it is one).
        bounds = r.split("-")
        if len(bounds) == 1 and bounds[0].isdigit():
            # It's a single number with no dash.
            scenario = int(bounds[0])
            ret[scenario] = True
            continue
        if len(bounds) == 2 and bounds[0].isdigit() and bounds[1].isdigit():
            # It's two numbers separated by a dash.
            for scenario in range(int(bounds[0]), int(bounds[1]) + 1):
                ret[scenario] = True
            continue
        # It's not valid syntax; give up.
        return None
    return ret

def restrictScenario(testcases, restrict):
    if restrict == '':
        return testcases
    else:
        scenarios = parse_int_list(restrict)
        if scenarios is not None:
            return [t for t in testcases
                if hasattr(t, 'scenario_number') and t.scenario_number in scenarios]
        else:
            return [t for t in testcases
                if hasattr(t, 'scenario_name') and t.scenario_name == restrict]

def addScenarioTests(tests, loader, testname, scenario):
    loaded = loader.loadTestsFromName(testname)
    tests.addTests(restrictScenario(generate_scenarios(loaded), scenario))

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

def testsFromArg(tests, loader, arg, scenario):
    # If a group of test is mentioned, do all tests in that group
    # e.g. 'run.py base'
    groupedfiles = glob.glob(suitedir + os.sep + 'test_' + arg + '*.py')
    if len(groupedfiles) > 0:
        for file in groupedfiles:
            testsFromArg(tests, loader, os.path.basename(file), scenario)
        return

    # Explicit test class names
    if not arg[0].isdigit():
        if arg.endswith('.py'):
            arg = arg[:-3]
        addScenarioTests(tests, loader, arg, scenario)
        return

    # Deal with ranges
    if '-' in arg:
        start, end = (int(a) for a in arg.split('-'))
    else:
        start, end = int(arg), int(arg)
    for t in xrange(start, end+1):
        addScenarioTests(tests, loader, 'test%03d' % t, scenario)

def error(exitval, prefix, msg):
    print('*** ERROR: {}: {}'.format(prefix, msg.replace('\n', '\n*** ')))
    sys.exit(exitval)

if __name__ == '__main__':
    # Turn numbers and ranges into test module names
    preserve = timestamp = debug = dryRun = gdbSub = lldbSub = longtest = zstdtest = ignoreStdout = False
    removeAtStart = True
    asan = False
    parallel = 0
    random_sample = 0
    batchtotal = batchnum = 0
    seed = seedw = seedz = 0
    configfile = None
    configwrite = False
    dirarg = None
    scenario = ''
    verbose = 1
    args = sys.argv[1:]
    testargs = []
    hook_names = []
    while len(args) > 0:
        arg = args.pop(0)
        from unittest import defaultTestLoader as loader

        # Command line options
        if arg[0] == '-':
            option = arg[1:]
            if option == '-asan':
                asan = True
                continue
            if option == '-batch' or option == 'b':
                if batchtotal != 0 or len(args) == 0:
                    usage()
                    sys.exit(2)
                # Batch expects an argument that has int slash int.
                # For example "-b 4/12"
                try:
                    left, right = args.pop(0).split('/')
                    batchnum = int(left)
                    batchtotal = int(right)
                except:
                    print('batch argument should be nnn/nnn')
                    usage()
                    sys.exit(2)
                if batchtotal <= 0 or batchnum < 0 or batchnum >= batchtotal:
                    usage()
                    sys.exit(2)
                continue
            if option == '-dir' or option == 'D':
                if dirarg != None or len(args) == 0:
                    usage()
                    sys.exit(2)
                dirarg = args.pop(0)
                continue
            if option == '-debug' or option == 'd':
                debug = True
                continue
            if option == '-dry-run' or option == 'n':
                dryRun = True
                continue
            if option == '-gdb' or option == 'g':
                gdbSub = True
                continue
            if option == '-lldb':
                lldbSub = True
                continue
            if option == '-help' or option == 'h':
                usage()
                sys.exit(0)
            if option == '-hook':
                if len(args) == 0:
                    usage()
                    sys.exit(2)
                hook_names.append(args.pop(0))
                continue
            if option == '-long' or option == 'l':
                longtest = True
                continue
            if option == '-zstd' or option == 'z':
                zstdtest = True
                continue
            if option == '-noremove':
                removeAtStart = False
                continue
            if option == '-random-sample' or option == 'r':
                if len(args) == 0:
                    usage()
                    sys.exit(2)
                random_sample = int(args.pop(0))
                if random_sample < 2 or random_sample > 1000:
                    usage()
                    sys.exit(2)
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
            if option == '-scenario' or option == 's':
                if scenario != '' or len(args) == 0:
                    usage()
                    sys.exit(2)
                scenario = args.pop(0)
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
            if option == '--ignore-stdout' or option == 'i':
                ignoreStdout = True
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
            if option == '-randomseed' or option == 'R':
                seedw = random.randint(1, 0xffffffff)
                seedz = random.randint(1, 0xffffffff)
                continue
            if option == '-seed' or option == 'S':
                if seed != 0 or len(args) == 0:
                    usage()
                    sys.exit(2)
                seed = args.pop(0)
                [seedw, seedz] = seed.split('.')
                if seedw == 0 or seedz == 0:
                    usage()
                    sys.exit(2)
                continue
            print('unknown arg: ' + arg)
            usage()
            sys.exit(2)
        testargs.append(arg)

    if asan:
        # To run ASAN, we need to ensure these environment variables are set:
        #    ASAN_SYMBOLIZER_PATH    full path to the llvm-symbolizer program
        #    LD_LIBRARY_PATH         includes path with wiredtiger shared object
        #    LD_PRELOAD              includes the ASAN runtime library
        #
        # Note that LD_LIBRARY_PATH has already been set above. The trouble with
        # simply setting these variables in the Python environment is that it's
        # too late. LD_LIBRARY_PATH is commonly cached by the shared library
        # loader at program startup, and that's already been done before Python
        # begins execution. Likewise, any preloading indicated by LD_PRELOAD
        # has already been done.
        #
        # Our solution is to set the variables as appropriate, and then restart
        # Python with the same argument list. The shared library loader will
        # have everything it needs on the second go round.
        #
        # Note: If the ASAN stops the program with the error:
        #    Shadow memory range interleaves with an existing memory mapping.
        #    ASan cannot proceed correctly.
        #
        # try rebuilding with the clang options:
        #    "-mllvm -asan-force-dynamic-shadow=1"
        # and make sure that clang is used for all compiles.
        #
        # We'd like to show this as a message, but there's no good way to
        # detect this error from here short of capturing/parsing all output
        # from the test run.
        ASAN_ENV = "__WT_TEST_SUITE_ASAN"    # if set, we've been here before
        ASAN_SYMBOLIZER_PROG = "llvm-symbolizer"
        ASAN_SYMBOLIZER_ENV = "ASAN_SYMBOLIZER_PATH"
        LD_PRELOAD_ENV = "LD_PRELOAD"
        SO_FILE_NAME = "libclang_rt.asan-x86_64.so"
        if not os.environ.get(ASAN_ENV):
            if verbose >= 2:
                print('Enabling ASAN environment and rerunning python')
            os.environ[ASAN_ENV] = "1"
            show_env(verbose, "LD_LIBRARY_PATH")
            if not os.environ.get(ASAN_SYMBOLIZER_ENV):
                os.environ[ASAN_SYMBOLIZER_ENV] = which(ASAN_SYMBOLIZER_PROG)
            if not os.environ.get(ASAN_SYMBOLIZER_ENV):
                error(ASAN_SYMBOLIZER_ENV,
                      'symbolizer program not found in PATH')
            show_env(verbose, ASAN_SYMBOLIZER_ENV)
            if not os.environ.get(LD_PRELOAD_ENV):
                symbolizer = follow_symlinks(os.environ[ASAN_SYMBOLIZER_ENV])
                bindir = os.path.dirname(symbolizer)
                sofiles = []
                if os.path.basename(bindir) == 'bin':
                    libdir = os.path.join(os.path.dirname(bindir), 'lib')
                    sofiles = find(libdir, SO_FILE_NAME)
                if len(sofiles) != 1:
                    if len(sofiles) == 0:
                        fmt = 'ASAN shared library file not found.\n' + \
                          'Set {} to the file location and rerun.'
                        error(3, SO_FILE_NAME, fmt.format(LD_PRELOAD_ENV))
                    else:
                        fmt = 'multiple ASAN shared library files found\n' + \
                          'under {}, expected just one.\n' + \
                          'Set {} to the correct file location and rerun.'
                        error(3, SO_FILE_NAME, fmt.format(libdir, LD_PRELOAD_ENV))
                os.environ[LD_PRELOAD_ENV] = sofiles[0]
            show_env(verbose, LD_PRELOAD_ENV)

            # Restart python!
            python = sys.executable
            os.execl(python, python, *sys.argv)
        elif verbose >= 2:
            print('Python restarted for ASAN')

    # We don't import wttest until after ASAN environment variables are set.
    import wttest
    # Use the same version of unittest found by wttest.py
    unittest = wttest.unittest
    tests = unittest.TestSuite()
    from testscenarios.scenarios import generate_scenarios

    import wthooks
    hookmgr = wthooks.WiredTigerHookManager(hook_names)
    # All global variables should be set before any test classes are loaded.
    # That way, verbose printing can be done at the class definition level.
    wttest.WiredTigerTestCase.globalSetup(preserve, removeAtStart, timestamp, gdbSub, lldbSub,
                                          verbose, wt_builddir, dirarg, longtest, zstdtest,
                                          ignoreStdout, seedw, seedz, hookmgr)

    # Without any tests listed as arguments, do discovery
    if len(testargs) == 0:
        if scenario != '':
            sys.stderr.write(
                'run.py: specifying a scenario requires a test name\n')
            usage()
            sys.exit(2)
        from discover import defaultTestLoader as loader
        suites = loader.discover(suitedir)
        suites = sorted(suites, key=lambda c: str(list(c)[0]))
        if configfile != None:
            suites = configApply(suites, configfile, configwrite)
        tests.addTests(restrictScenario(generate_scenarios(suites), ''))
    else:
        for arg in testargs:
            testsFromArg(tests, loader, arg, scenario)

    tests = hookmgr.filter_tests(tests)
    # Shuffle the tests and create a new suite containing every Nth test from
    # the original suite
    if random_sample > 0:
        random_sample_tests = []
        for test in tests:
            random_sample_tests.append(test)
        random.shuffle(random_sample_tests)
        tests = unittest.TestSuite(random_sample_tests[::random_sample])
    if debug:
        import pdb
        pdb.set_trace()
    if batchtotal != 0:
        # For test batching, we want to split up all the tests evenly, and
        # spread out the tests, so each batch contains tests of all kinds. We'd
        # like to prioritize the lowest scenario numbers first, so if there's a
        # failure, we won't have to do all X thousand of some test's scenarios
        # before we see a failure in the next test.  To that end, we define a
        # sort function that sorts by scenario first, and test name second.
        hugetests = set()
        def get_sort_keys(test):
            s = 0
            name = test.simpleName()
            if hasattr(test, 'scenario_number'):
                s = test.scenario_number
                if s > 1000:
                    hugetests.add(name)    # warn for too many scenarios
            return (s, test.simpleName())  # sort by scenario number first
        all_tests = sorted(tests, key = get_sort_keys)
        if not longtest:
            for name in hugetests:
                print("WARNING: huge test " + name + " has > 1000 scenarios.\n" +
                      "That is only appropriate when using the --long option.\n" +
                      "The number of scenarios for the test should be pruned")

        # At this point we have an ordered list of all the tests.
        # Break it into just our batch.
        tests = unittest.TestSuite(all_tests[batchnum::batchtotal])
    if dryRun:
        for line in tests:
            print(line)
    else:
        result = wttest.runsuite(tests, parallel)
        sys.exit(0 if result.wasSuccessful() else 1)

    sys.exit(0)
