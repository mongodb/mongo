#!/usr/bin/env python

# smoke.py: run some mongo tests.

# Bugs, TODOs:

# 0 Some tests hard-code pathnames relative to the mongo repository,
#   so the smoke.py process and all its children must be run with the
#   mongo repo as current working directory.  That's kinda icky.

# 1 The tests that are implemented as standalone executables ("test",
#   "perftest"), don't take arguments for the dbpath, but
#   unconditionally use "/tmp/unittest".

# 2 mongod output gets intermingled with mongo output, and it's often
#   hard to find error messages in the slop.  Maybe have smoke.py do
#   some fancier wrangling of child process output?

# 3 Some test suites run their own mongods, and so don't need us to
#   run any mongods around their execution.  (It's harmless to do so,
#   but adds noise in the output.)

# 4 Running a separate mongo shell for each js file is slower than
#   loading js files into one mongo shell process.  Maybe have runTest
#   queue up all filenames ending in ".js" and run them in one mongo
#   shell at the "end" of testing?

# 5 Right now small-oplog implies master/slave replication.  Maybe
#   running with replication should be an orthogonal concern.  (And
#   maybe test replica set replication, too.)

# 6 We use cleanbb.py to clear out the dbpath, but cleanbb.py kills
#   off all mongods on a box, which means you can't run two smoke.py
#   jobs on the same host at once.  So something's gotta change.

from datetime import datetime
import glob
from optparse import OptionParser
import os
import parser
import re
import shutil
import socket
from subprocess import (Popen,
                        PIPE,
                        call)
import sys
import time

from pymongo import Connection
from pymongo.errors import OperationFailure

import utils

try:
    import cPickle as pickle
except ImportError:
    import pickle

# TODO clean this up so we don't need globals...
mongo_repo = os.getcwd() #'./'
failfile = os.path.join(mongo_repo, 'failfile.smoke')
test_path = None
mongod_executable = None
mongod_port = None
shell_executable = None
continue_on_failure = None

tests = []
winners = []
losers = {}
fails = [] # like losers but in format of tests

# For replication hash checking
replicated_collections = []
lost_in_slave = []
lost_in_master = []
screwy_in_slave = {}

smoke_db_prefix = ''
small_oplog = False

# This class just implements the with statement API, for a sneaky
# purpose below.
class Nothing(object):
    def __enter__(self):
        return self
    def __exit__(self, type, value, traceback):
        return not isinstance(value, Exception)

def buildlogger(cmd, is_global=False):
    # if the environment variable MONGO_USE_BUILDLOGGER
    # is set to 'true', then wrap the command with a call
    # to buildlogger.py, which sends output to the buidlogger
    # machine; otherwise, return as usual.
    if os.environ.get('MONGO_USE_BUILDLOGGER', '').lower().strip() == 'true':
        if is_global:
            return [utils.find_python(), 'buildscripts/buildlogger.py', '-g'] + cmd
        else:
            return [utils.find_python(), 'buildscripts/buildlogger.py'] + cmd
    return cmd


class mongod(object):
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.proc = None
        self.auth = False

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, type, value, traceback):
        try:
            self.stop()
        except Exception, e:
            print >> sys.stderr, "error shutting down mongod"
            print >> sys.stderr, e
        return not isinstance(value, Exception)

    def ensure_test_dirs(self):
        utils.ensureDir(smoke_db_prefix + "/tmp/unittest/")
        utils.ensureDir(smoke_db_prefix + "/data/")
        utils.ensureDir(smoke_db_prefix + "/data/db/")

    def check_mongo_port(self, port=27017):
        sock = socket.socket()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(1)
        sock.connect(("localhost", int(port)))
        sock.close()

    def did_mongod_start(self, port=mongod_port, timeout=300):
        while timeout > 0:
            time.sleep(1)
            try:
                self.check_mongo_port(int(port))
                return True
            except Exception,e:
                print >> sys.stderr, e
                timeout = timeout - 1
        print >> sys.stderr, "timeout starting mongod"
        return False

    def setup_admin_user(self, port=mongod_port):
        try:
            Connection( "localhost" , int(port) ).admin.add_user("admin","password")
        except OperationFailure, e:
            if e.message == 'need to login':
                pass # SERVER-4225
            else:
                raise e

    def start(self):
        global mongod_port
        global mongod
        if self.proc:
            print >> sys.stderr, "probable bug: self.proc already set in start()"
            return
        self.ensure_test_dirs()
        dir_name = smoke_db_prefix + "/data/db/sconsTests/"
        self.port = int(mongod_port)
        self.slave = False
        if 'slave' in self.kwargs:
            dir_name = smoke_db_prefix + '/data/db/sconsTestsSlave/'
            srcport = mongod_port
            self.port += 1
            self.slave = True
        if os.path.exists(dir_name):
            if 'slave' in self.kwargs:
                argv = [utils.find_python(), "buildscripts/cleanbb.py", '--nokill', dir_name]
            else:
                argv = [utils.find_python(), "buildscripts/cleanbb.py", dir_name]
            call(argv)
        utils.ensureDir(dir_name)
        argv = [mongod_executable, "--port", str(self.port), "--dbpath", dir_name]
        if self.kwargs.get('small_oplog'):
            argv += ["--master", "--oplogSize", "511"]
        if self.slave:
            argv += ['--slave', '--source', 'localhost:' + str(srcport)]
        if self.kwargs.get('no_journal'):
            argv += ['--nojournal']
        if self.kwargs.get('no_preallocj'):
            argv += ['--nopreallocj']
        if self.kwargs.get('auth'):
            argv += ['--auth']
            self.auth = True
        print "running " + " ".join(argv)
        self.proc = Popen(buildlogger(argv, is_global=True))
        if not self.did_mongod_start(self.port):
            raise Exception("Failed to start mongod")

        if self.auth:
            self.setup_admin_user(self.port)

        if self.slave:
            local = Connection(port=self.port, slave_okay=True).local
            synced = False
            while not synced:
                synced = True
                for source in local.sources.find(fields=["syncedTo"]):
                    synced = synced and "syncedTo" in source and source["syncedTo"]

    def stop(self):
        if not self.proc:
            print >> sys.stderr, "probable bug: self.proc unset in stop()"
            return
        try:
            # This function not available in Python 2.5
            self.proc.terminate()
        except AttributeError:
            if os.sys.platform == "win32":
                import win32process
                win32process.TerminateProcess(self.proc._handle, -1)
            else:
                from os import kill
                kill(self.proc.pid, 15)
        self.proc.wait()
        sys.stderr.flush()
        sys.stdout.flush()

    def wait_for_repl(self):
        Connection(port=self.port).test.smokeWait.insert({}, w=2, wtimeout=5*60*1000)

class Bug(Exception):
    def __str__(self):
        return 'bug in smoke.py: ' + super(Bug, self).__str__()

class TestFailure(Exception):
    pass

class TestExitFailure(TestFailure):
    def __init__(self, *args):
        self.path = args[0]

        self.status=args[1]
    def __str__(self):
        return "test %s exited with status %d" % (self.path, self.status)

class TestServerFailure(TestFailure):
    def __init__(self, *args):
        self.path = args[0]
        self.status = -1 # this is meaningless as an exit code, but
                         # that's the point.
    def __str__(self):
        return 'mongod not running after executing test %s' % self.path

def check_db_hashes(master, slave):
    # Need to pause a bit so a slave might catch up...
    if not slave.slave:
        raise(Bug("slave instance doesn't have slave attribute set"))

    print "waiting for slave to catch up"
    master.wait_for_repl()
    print "caught up!"

    # FIXME: maybe make this run dbhash on all databases?
    for mongod in [master, slave]:
        mongod.dbhash = Connection(port=mongod.port, slave_okay=True).test.command("dbhash")
        mongod.dict = mongod.dbhash["collections"]

    global lost_in_slave, lost_in_master, screwy_in_slave, replicated_collections

    replicated_collections += master.dict.keys()
    
    for db in replicated_collections:
        if db not in slave.dict:
            lost_in_slave.append(db)
        mhash = master.dict[db]
        shash = slave.dict[db]
        if mhash != shash:
            screwy_in_slave[db] = mhash + "/" + shash

    for db in slave.dict.keys():
        if db not in master.dict:
            lost_in_master.append(db)


def ternary( b , l="true", r="false" ):
    if b:
        return l
    return r


# Blech.
def skipTest(path):
    basename = os.path.basename(path)
    parentDir = os.path.basename(os.path.dirname(path))
    if small_oplog: # For tests running in parallel
        if basename in ["cursor8.js", "indexh.js", "dropdb.js"]:
            return True
    if auth or keyFile: # For tests running with auth
        return os.path.join(parentDir,basename) in ["sharding/sync3.js", "sharding/sync6.js"]
    return False

def runTest(test):
    # test is a tuple of ( filename , usedb<bool> )
    # filename should be a js file to run
    # usedb is true if the test expects a mongod to be running

    (path, usedb) = test
    (ignore, ext) = os.path.splitext(path)
    if skipTest(path):
        print "skipping " + path
        return
    if ext == ".js":
        argv = [shell_executable, "--port", mongod_port]
        if not usedb:
            argv += ["--nodb"]
        if small_oplog:
            argv += ["--eval", 'testingReplication = true;']
        argv += [path]
    elif ext in ["", ".exe"]:
        # Blech.
        if os.path.basename(path) in ["test", "test.exe", "perftest", "perftest.exe"]:
            argv = [path]
        # more blech
        elif os.path.basename(path) in ['mongos', 'mongos.exe']:
            argv = [path, "--test"]
        else:
            argv = [test_path and os.path.abspath(os.path.join(test_path, path)) or path,
                    "--port", mongod_port]
    else:
        raise Bug("fell off in extenstion case: %s" % path)

    if keyFile:
        f = open(keyFile, 'r')
        keyFileData = re.sub(r'\s', '', f.read()) # Remove all whitespace
        f.close()
    else:
        keyFileData = None


    # sys.stdout.write() is more atomic than print, so using it prevents
    # lines being interrupted by, e.g., child processes
    sys.stdout.write(" *******************************************\n")
    sys.stdout.write("         Test : %s ...\n" % os.path.basename(path))
    sys.stdout.flush()

    # FIXME: we don't handle the case where the subprocess
    # hangs... that's bad.
    if ( argv[0].endswith( 'mongo' ) or argv[0].endswith( 'mongo.exe' ) ) and not '--eval' in argv :
        evalString = 'TestData = new Object();' + \
                     'TestData.testPath = "' + path + '";' + \
                     'TestData.testFile = "' + os.path.basename( path ) + '";' + \
                     'TestData.testName = "' + re.sub( ".js$", "", os.path.basename( path ) ) + '";' + \
                     'TestData.noJournal = ' + ternary( no_journal )  + ";" + \
                     'TestData.noJournalPrealloc = ' + ternary( no_preallocj )  + ";" + \
                     'TestData.auth = ' + ternary( auth ) + ";" + \
                     'TestData.keyFile = ' + ternary( keyFile , '"' + str(keyFile) + '"' , 'null' ) + ";" + \
                     'TestData.keyFileData = ' + ternary( keyFile , '"' + str(keyFileData) + '"' , 'null' ) + ";"
        if os.sys.platform == "win32":
            # double quotes in the evalString on windows; this
            # prevents the backslashes from being removed when
            # the shell (i.e. bash) evaluates this string. yuck.
            evalString = evalString.replace('\\', '\\\\')

        if auth and usedb:
            evalString += 'jsTest.authenticate(db.getMongo());'

        argv = argv + [ '--eval', evalString]
    
    if argv[0].endswith( 'test' ) and no_preallocj :
        argv = argv + [ '--nopreallocj' ]
    
    
    sys.stdout.write("      Command : %s\n" % ' '.join(argv))
    sys.stdout.write("         Date : %s\n" % datetime.now().ctime())
    sys.stdout.flush()

    os.environ['MONGO_TEST_FILENAME'] = os.path.basename(path)
    t1 = time.time()
    r = call(buildlogger(argv), cwd=test_path)
    t2 = time.time()
    del os.environ['MONGO_TEST_FILENAME']

    sys.stdout.write("                %fms\n" % ((t2 - t1) * 1000))
    sys.stdout.flush()

    if r != 0:
        raise TestExitFailure(path, r)
    
    try:
        c = Connection( "127.0.0.1" , int(mongod_port) )
    except Exception,e:
        raise TestServerFailure(path)

    print ""

def run_tests(tests):
    # FIXME: some suites of tests start their own mongod, so don't
    # need this.  (So long as there are no conflicts with port,
    # dbpath, etc., and so long as we shut ours down properly,
    # starting this mongod shouldn't break anything, though.)
    
    # The reason we want to use "with" is so that we get __exit__ semantics
    # but "with" is only supported on Python 2.5+

    master = mongod(small_oplog=small_oplog,no_journal=no_journal,no_preallocj=no_preallocj,auth=auth).__enter__()
    try:
        if small_oplog:
            slave = mongod(slave=True).__enter__()
        else:
            slave = Nothing()
        try:
            if small_oplog:
                master.wait_for_repl()

            for test in tests:
                try:
                    fails.append(test)
                    runTest(test)
                    fails.pop()
                    winners.append(test)

                    if small_oplog:
                        master.wait_for_repl()
                except TestFailure, f:
                    try:
                        print f
                        # Record the failing test and re-raise.
                        losers[f.path] = f.status
                        raise f
                    except TestServerFailure, f:
                        return 2
                    except TestFailure, f:
                        if not continue_on_failure:
                            return 1
            if isinstance(slave, mongod):
                check_db_hashes(master, slave)
        finally:
            slave.__exit__(None, None, None)
    finally:
        master.__exit__(None, None, None)
    return 0


def report():
    print "%d tests succeeded" % len(winners)
    num_missed = len(tests) - (len(winners) + len(losers.keys()))
    if num_missed:
        print "%d tests didn't get run" % num_missed
    if losers:
        print "The following tests failed (with exit code):"
        for loser in losers:
            print "%s\t%d" % (loser, losers[loser])

    def missing(lst, src, dst):
        if lst:
            print """The following collections were present in the %s but not the %s
at the end of testing:""" % (src, dst)
            for db in lst:
                print db
    missing(lost_in_slave, "master", "slave")
    missing(lost_in_master, "slave", "master")
    if screwy_in_slave:
        print """The following collections has different hashes in master and slave
at the end of testing:"""
        for db in screwy_in_slave.keys():
            print "%s\t %s" % (db, screwy_in_slave[db])
    if small_oplog and not (lost_in_master or lost_in_slave or screwy_in_slave):
        print "replication ok for %d collections" % (len(replicated_collections))
    if losers or lost_in_slave or lost_in_master or screwy_in_slave:
        raise Exception("Test failures")


def expand_suites(suites):
    globstr = None
    tests = []
    for suite in suites:
        if suite == 'all':
            return expand_suites(['test', 'perf', 'client', 'js', 'jsPerf', 'jsSlowNightly', 'jsSlowWeekly', 'clone', 'parallel', 'repl', 'auth', 'sharding', 'tool'])
        if suite == 'test':
            if os.sys.platform == "win32":
                program = 'test.exe'
            else:
                program = 'test'
            (globstr, usedb) = (program, False)
        elif suite == 'perf':
            if os.sys.platform == "win32":
                program = 'perftest.exe'
            else:
                program = 'perftest'
            (globstr, usedb) = (program, False)
        elif suite == 'client':
            paths = ["firstExample", "secondExample", "whereExample", "authTest", "clientTest", "httpClientTest"]
            if os.sys.platform == "win32":
                paths = [path + '.exe' for path in paths]
            # hack
            tests += [(test_path and path or os.path.join(mongo_repo, path), False) for path in paths]
        elif suite == 'mongosTest':
            if os.sys.platform == "win32":
                program = 'mongos.exe'
            else:
                program = 'mongos'
            tests += [(os.path.join(mongo_repo, program), False)]
        elif os.path.exists( suite ):
            tests += [ ( os.path.join( mongo_repo , suite ) , True ) ]
        else:
            try:
                globstr, usedb = {"js": ("[!_]*.js", True),
                                  "quota": ("quota/*.js", True),
                                  "jsPerf": ("perf/*.js", True),
                                  "disk": ("disk/*.js", True),
                                  "jsSlowNightly": ("slowNightly/*.js", True),
                                  "jsSlowWeekly": ("slowWeekly/*.js", False),
                                  "parallel": ("parallel/*.js", True),
                                  "clone": ("clone/*.js", False),
                                  "repl": ("repl/*.js", False),
                                  "replSets": ("replsets/*.js", False),
                                  "dur": ("dur/*.js", False),
                                  "auth": ("auth/*.js", False),
                                  "sharding": ("sharding/*.js", False),
                                  "tool": ("tool/*.js", False),
                                  "aggregation": ("aggregation/test[ab]*.js", True),
                                 }[suite]
            except KeyError:
                raise Exception('unknown test suite %s' % suite)

        if globstr:
            if globstr.endswith('.js'):
                loc = 'jstests/'
            else:
                loc = ''
            globstr = os.path.join(mongo_repo, (os.path.join(loc, globstr)))
            globstr = os.path.normpath(globstr)
            paths = glob.glob(globstr)
            paths.sort()
            tests += [(path, usedb) for path in paths]

    return tests

def add_exe(e):
    if os.sys.platform.startswith( "win" ) and not e.endswith( ".exe" ):
        e += ".exe"
    return e

def set_globals(options):
    global mongod_executable, mongod_port, shell_executable, continue_on_failure, small_oplog, no_journal, no_preallocj, auth, keyFile, smoke_db_prefix, test_path
    #Careful, this can be called multiple times
    test_path = options.test_path

    mongod_executable = add_exe(options.mongod_executable)
    if not os.path.exists(mongod_executable):
        raise Exception("no mongod found in this directory.")

    mongod_port = options.mongod_port

    shell_executable = add_exe( options.shell_executable )
    if not os.path.exists(shell_executable):
        raise Exception("no mongo shell found in this directory.")

    continue_on_failure = options.continue_on_failure
    smoke_db_prefix = options.smoke_db_prefix
    small_oplog = options.small_oplog
    no_journal = options.no_journal
    no_preallocj = options.no_preallocj
    auth = options.auth
    keyFile = options.keyFile

def clear_failfile():
    if os.path.exists(failfile):
        os.remove(failfile)

def run_old_fails():
    global tests

    try:
        f = open(failfile, 'r')
        testsAndOptions = pickle.load(f)
        f.close()
    except Exception:
        try:
            f.close()
        except:
            pass
        clear_failfile()
        return # This counts as passing so we will run all tests

    tests = [x[0] for x in testsAndOptions]
    passed = []
    try:
        for (i, (test, options)) in enumerate(testsAndOptions):
            # SERVER-5102: until we can figure out a better way to manage
            # dependencies of the --only-old-fails build phase, just skip
            # tests which we can't safely run at this point
            path, usedb = test
            filename = os.path.basename(path)
            if filename in ('test', 'test.exe') or filename.endswith('.js'):
                set_globals(options)
                oldWinners = len(winners)
                run_tests([test])
                if len(winners) != oldWinners: # can't use return value due to continue_on_failure
                    passed.append(i)
    finally:
        for offset, i in enumerate(passed):
            testsAndOptions.pop(i - offset)

        if testsAndOptions:
            f = open(failfile, 'w')
            pickle.dump(testsAndOptions, f)
        else:
            clear_failfile()

        report() # exits with failure code if there is an error

def add_to_failfile(tests, options):
    try:
        f = open(failfile, 'r')
        testsAndOptions = pickle.load(f)
    except Exception:
        testsAndOptions = []

    for test in tests:
        if (test, options) not in testsAndOptions:
            testsAndOptions.append( (test, options) )

    f = open(failfile, 'w')
    pickle.dump(testsAndOptions, f)



def main():
    global mongod_executable, mongod_port, shell_executable, continue_on_failure, small_oplog, no_journal, no_preallocj, auth, keyFile, smoke_db_prefix, test_path
    parser = OptionParser(usage="usage: smoke.py [OPTIONS] ARGS*")
    parser.add_option('--mode', dest='mode', default='suite',
                      help='If "files", ARGS are filenames; if "suite", ARGS are sets of tests (%default)')
    # Some of our tests hard-code pathnames e.g., to execute, so until
    # that changes we don't have the freedom to run from anyplace.
    # parser.add_option('--mongo-repo', dest='mongo_repo', default=None,
    parser.add_option('--test-path', dest='test_path', default=None,
                      help="Path to the test executables to run, "
                      "currently only used for 'client' (%default)")
    parser.add_option('--mongod', dest='mongod_executable', default=os.path.join(mongo_repo, 'mongod'),
                      help='Path to mongod to run (%default)')
    parser.add_option('--port', dest='mongod_port', default="27999",
                      help='Port the mongod will bind to (%default)')
    parser.add_option('--mongo', dest='shell_executable', default=os.path.join(mongo_repo, 'mongo'),
                      help='Path to mongo, for .js test files (%default)')
    parser.add_option('--continue-on-failure', dest='continue_on_failure',
                      action="store_true", default=False,
                      help='If supplied, continue testing even after a test fails')
    parser.add_option('--from-file', dest='File',
                      help="Run tests/suites named in FILE, one test per line, '-' means stdin")
    parser.add_option('--smoke-db-prefix', dest='smoke_db_prefix', default=smoke_db_prefix,
                      help="Prefix to use for the mongods' dbpaths ('%default')")
    parser.add_option('--small-oplog', dest='small_oplog', default=False,
                      action="store_true",
                      help='Run tests with master/slave replication & use a small oplog')
    parser.add_option('--nojournal', dest='no_journal', default=False,
                      action="store_true",
                      help='Do not turn on journaling in tests')
    parser.add_option('--nopreallocj', dest='no_preallocj', default=False,
                      action="store_true",
                      help='Do not preallocate journal files in tests')
    parser.add_option('--auth', dest='auth', default=False,
                      action="store_true",
                      help='Run standalone mongods in tests with authentication enabled')
    parser.add_option('--keyFile', dest='keyFile', default=None,
                      help='Path to keyFile to use to run replSet and sharding tests with authentication enabled')
    parser.add_option('--ignore', dest='ignore_files', default=None,
                      help='Pattern of files to ignore in tests')
    parser.add_option('--only-old-fails', dest='only_old_fails', default=False,
                      action="store_true",
                      help='Check the failfile and only run all tests that failed last time')
    parser.add_option('--reset-old-fails', dest='reset_old_fails', default=False,
                      action="store_true",
                      help='Clear the failfile. Do this if all tests pass')

    global tests
    (options, tests) = parser.parse_args()

    set_globals(options)

    if options.File:
        if options.File == '-':
            tests = sys.stdin.readlines()
        else:
            f = open(options.File)
            tests = f.readlines()
    tests = [t.rstrip('\n') for t in tests]

    if options.only_old_fails:
        run_old_fails()
        return
    elif options.reset_old_fails:
        clear_failfile()
        return

    # If we're in suite mode, tests is a list of names of sets of tests.
    if options.mode == 'suite':
        tests = expand_suites(tests)
    elif options.mode == 'files':
        tests = [(os.path.abspath(test), True) for test in tests]

    if options.ignore_files != None :
        ignore_patt = re.compile( options.ignore_files )
        tests = filter( lambda x : ignore_patt.search( x[0] ) == None, tests )

    if not tests:
        raise Exception( "no tests specified" )

    try:
        run_tests(tests)
    finally:
        add_to_failfile(fails, options)
        report()


if __name__ == "__main__":
    main()
