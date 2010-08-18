#!/usr/bin/python

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

from __future__ import with_statement
from subprocess import Popen, PIPE, call
import os
import sys
import utils
import time
import socket
from optparse import OptionParser
import atexit
import glob
import shutil
import re
import parser

mongoRepo = os.getcwd() #'./'
testPath = None

mongodExecutable = "./mongod"
mongodPort = "32000"
shellExecutable = "./mongo"
continueOnFailure = False
oneMongodPerTest = False

tests = []
winners = []
losers = {}

# Finally, atexit functions seem to be a little oblivious to whether
# Python is exiting because of an error, so we'll use this to
# communicate with the report() function.
exit_bad = True

# For replication hash checking
replicated_dbs = []
lost_in_slave = []
lost_in_master = []
screwy_in_slave = {}

smokeDbPrefix = ''
smallOplog = False

# This class just implements the with statement API, for a sneaky
# purpose below.
class nothing(object):
    def __enter__(self):
        return self
    def __exit__(self, type, value, traceback):
        return not isinstance(value, Exception)

class mongod(object):
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.proc = None

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

    def ensureTestDirs(self):
        utils.ensureDir( smokeDbPrefix + "/tmp/unittest/" )
        utils.ensureDir( smokeDbPrefix + "/data/" )
        utils.ensureDir( smokeDbPrefix + "/data/db/" )

    def checkMongoPort( self, port=27017 ):
        sock = socket.socket()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(1)
        sock.connect(("localhost", int(port)))
        sock.close()
    
    def didMongodStart( self, port=mongodPort, timeout=20 ):
        while timeout > 0:
            time.sleep( 1 )
            try:
                self.checkMongoPort( int(port) )
                return True
            except Exception,e:
                print >> sys.stderr, e
                timeout = timeout - 1
        return False

    def start(self):
        global mongodPort
        global mongod
        if self.proc:
            print >> sys.stderr, "probable bug: self.proc already set in start()"
            return
        self.ensureTestDirs()
        dirName = smokeDbPrefix + "/data/db/sconsTests/"
        self.port = int(mongodPort)
        self.slave = False
        if 'slave' in self.kwargs:
            dirName = smokeDbPrefix + '/data/db/sconsTestsSlave/'
            srcport = mongodPort
            self.port += 1
            self.slave = True
        if os.path.exists ( dirName ):
            if 'slave' in self.kwargs:
                argv = ["python", "buildscripts/cleanbb.py", '--nokill', dirName] 

            else:
                argv = ["python", "buildscripts/cleanbb.py", dirName]
            call( argv )
        utils.ensureDir( dirName )
        argv = [mongodExecutable, "--port", str(self.port), "--dbpath", dirName]
        if self.kwargs.get('smallOplog'):
            argv += ["--master", "--oplogSize", "10"]
        if self.slave:
            argv += ['--slave', '--source', 'localhost:'+str(srcport)]
        print "running " + " ".join(argv)
        self.proc = Popen(argv)
        if not self.didMongodStart( self.port ):
            raise Exception( "Failed to start mongod" )
        
        if self.slave:
            while True:
                argv = [shellExecutable, "--port", str(self.port), "--quiet", "--eval", 'db.printSlaveReplicationInfo()']
                res = Popen(argv, stdout=PIPE).communicate()[0]
                if res.find('initial sync') < 0:
                    break
            

            
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
                kill( self.proc.pid, 15 )
        self.proc.wait()
        sys.stderr.flush()
        sys.stdout.flush()
    
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

def checkDbHashes(master, slave):
    # Need to pause a bit so a slave might catch up...
    if not slave.slave:
        raise(Bug("slave instance doesn't have slave attribute set"))

    print "waiting for slave to catch up..."
    ARB=10  # ARBITRARY
    time.sleep(ARB)
    argv = [shellExecutable, "--port", str(slave.port), "--quiet", "--eval", 'db.getLastError(2, 30000)']
    res = Popen(argv, stdout=PIPE).wait() #.communicate()[0]
    # FIXME: maybe assert that that res == 0?

    # FIXME: maybe make this run dbhash on all databases?
    for mongod in [master, slave]:
        argv = [shellExecutable, "--port", str(mongod.port), "--quiet", "--eval", "x=db.runCommand('dbhash'); printjson(x.collections)"]
        hashstr = Popen(argv, stdout=PIPE).communicate()[0]
        # WARNING FIXME KLUDGE et al.: this is sleazy and unsafe.
        mongod.dict = eval(hashstr)

    global lost_in_slave, lost_in_master, screwy_in_slave, replicated_dbs
    
    for db in replicated_dbs:
        if db not in slave.dict:
            lost_in_slave.append(db)
        mhash = master.dict[db]
        shash = slave.dict[db]
        if mhash != shash:
            screwy_in_slave[db] = mhash + "/" + shash
    for db in slave.dict.keys():
        if db not in master.dict:
            lost_in_master.append(db)
    replicated_dbs += master.dict.keys()

# Blech.
def skipTest(path):
    if smallOplog:
        if os.path.basename(path) in ["cursor8.js", "indexh.js"]:
            return True
    return False

def runTest(test):
    (path, usedb) = test
    (ignore, ext) = os.path.splitext(path)
    if skipTest(path):
        print "skippping " + path
        return
    if ext == ".js":
        argv=[shellExecutable, "--port", mongodPort]
        if not usedb:
            argv += ["--nodb"] 
        if smallOplog:
            argv += ["--eval", 'testingReplication = true;']
        argv += [path]
    elif ext in ["", ".exe"]:
        # Blech.
        if os.path.basename(path) in ["test", "test.exe", "perftest", "perftest.exe"]:
            argv=[path]
        # more blech
        elif os.path.basename(path) == 'mongos':
            argv=[path, "--test"]
        else:
            argv=[testPath and os.path.abspath(os.path.join(testPath, path)) or path,
                  "--port", mongodPort]
    else:
        raise Bug("fell off in extenstion case: %s" % path)
    print " *******************************************"
    print "         Test : " + os.path.basename(path) + " ..."
    t1=time.time()
    # FIXME: we don't handle the case where the subprocess
    # hangs... that's bad.
    r = call(argv, cwd=testPath)
    t2=time.time()
    print "                " + str((t2-t1)*1000) + "ms"
    if r != 0:
        raise TestExitFailure(path, r)
    if Popen( [ mongodExecutable, "msg", "ping", mongodPort ], stdout=PIPE ).communicate()[0].count( "****ok" ) == 0:
        raise TestServerFailure(path)
    if call( [ mongodExecutable, "msg", "ping", mongodPort ] ) != 0:
        raise TestServerFailure(path)
    print ""

def runTests(tests):
    # If we're in one-mongo-per-test mode, we instantiate a nothing
    # around the loop, and a mongod inside the loop.

    # FIXME: some suites of tests start their own mongod, so don't
    # need this.  (So long as there are no conflicts with port,
    # dbpath, etc., and so long as we shut ours down properly,
    # starting this mongod shouldn't break anything, though.)
    with nothing() if oneMongodPerTest else mongod(smallOplog=smallOplog) as master1:
        with nothing() if oneMongodPerTest else (mongod(slave=True) if smallOplog else nothing()) as slave1:
            for test in tests:
                try:
                    with mongod(smallOplog=smallOplog) if oneMongodPerTest else nothing() as master2: 
                        with mongod(slave=True) if oneMongodPerTest and smallOplog else nothing() as slave2:
                            runTest(test)
                    winners.append(test)
                    if isinstance(slave2, mongod):
                        checkDbHashes(master2, slave2)
                except TestFailure, f:
                    try:
                        print f
                        # Record the failing test and re-raise.
                        losers[f.path] = f.status
                        raise f
                    except TestServerFailure, f: 
                        if not oneMongodPerTest:
                            return 2
                    except TestFailure, f:
                        if not continueOnFailure:
                            return 1
            if isinstance(slave1, mongod):
                checkDbHashes(master1, slave1)

    return 0

def report():
    print "%d test%s succeeded" % (len(winners), '' if len(winners) == 1 else 's')
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
    if smallOplog and not (lost_in_master or lost_in_slave or screwy_in_slave):
        print "replication ok for %d collections" % (len(replicated_dbs))
    if (exit_bad or losers or lost_in_slave or lost_in_master or screwy_in_slave):
        status = 1
    else:
        status = 0
    exit (status)

def expandSuites(suites):
    globstr = None
    global mongoRepo, tests
    for suite in suites:
        if suite == 'smokeAll':
            tests = []
            expandSuites(['smoke', 'smokePerf', 'smokeClient', 'smokeJs', 'smokeJsPerf', 'smokeJsSlowNightly', 'smokeJsSlowWeekly', 'smokeParallel', 'smokeClone', 'smokeParallel', 'smokeRepl', 'smokeAuth', 'smokeSharding', 'smokeTool'])
            break
        if suite == 'smoke':
            if os.sys.platform == "win32":
                program = 'test.exe'
            else:
                program = 'test'
            (globstr, usedb) = (program, False)
        elif suite == 'smokePerf':
            if os.sys.platform == "win32":
                program = 'perftest.exe'
            else:
                program = 'perftest'
            (globstr, usedb) = (program, False)
        elif suite == 'smokeJs':
            # FIXME: _runner.js seems equivalent to "[!_]*.js".
            #(globstr, usedb) = ('_runner.js', True)
            (globstr, usedb) = ('[!_]*.js', True)
        elif suite == 'smokeQuota':
            (globstr, usedb) = ('quota/*.js', True)
        elif suite == 'smokeJsPerf':
            (globstr, usedb) = ('perf/*.js', True)
        elif suite == 'smokeDisk':
            (globstr, usedb) = ('disk/*.js', True)
        elif suite == 'smokeJsSlowNightly':
            (globstr, usedb) = ('slowNightly/*.js', True)
        elif suite == 'smokeJsSlowWeekly':
            (globstr, usedb) = ('slowWeekly/*.js', True)
        elif suite == 'smokeParallel':
            (globstr, usedb) = ('parallel/*.js', True)
        elif suite == 'smokeClone':
            (globstr, usedb) = ('clone/*.js', False)
        elif suite == 'smokeRepl':
            (globstr, usedb) = ('repl/*.js', False)
        elif suite == 'smokeReplSets':
            (globstr, usedb) = ('replsets/*.js', False)
        elif suite == 'smokeAuth':
            (globstr, usedb) = ('auth/*.js', False)
        elif suite == 'smokeSharding':
            (globstr, usedb) = ('sharding/*.js', False)
        elif suite == 'smokeTool':
            (globstr, usedb) = ('tool/*.js', False)
        # well, the above almost works for everything...
        elif suite == 'smokeClient':
            paths = ["firstExample", "secondExample", "whereExample", "authTest", "clientTest", "httpClientTest"]
            if os.sys.platform == "win32":
                paths = [path+'.exe' for path in paths]
            # hack
            tests += [(testPath and path or os.path.join(mongoRepo, path), False) for path in paths]
        elif suite == 'mongosTest':
            if os.sys.platform == "win32":
                program = 'mongos.exe'
            else:
                program = 'mongos'
            tests += [(os.path.join(mongoRepo, program), False)]
        else:
            raise Exception('unknown test suite %s' % suite)

        if globstr:
            globstr = os.path.join(mongoRepo, (os.path.join(('jstests/' if globstr.endswith('.js') else ''), globstr)))
            paths = glob.glob(globstr)
            paths.sort()
            tests += [(path, usedb) for path in paths]
    if not tests:
        raise Exception( "no tests found" )
    return tests

def main():
    parser = OptionParser(usage="usage: smoke.py [OPTIONS] ARGS*")
    parser.add_option('--mode', dest='mode', default='suite',
                      help='If "files", ARGS are filenames; if "suite", ARGS are sets of tests.  (default "suite")')
    # Some of our tests hard-code pathnames e.g., to execute, so until
    # th we don't have the freedom to run from anyplace.
#    parser.add_option('--mongo-repo', dest='mongoRepo', default=None,
#                      help='Top-level directory of mongo checkout to use.  (default: script will make a guess)')
    parser.add_option('--test-path', dest='testPath', default=None,
                      help="Path to the test executables to run "
                      "(currently only used for smokeClient)")
    parser.add_option('--mongod', dest='mongodExecutable', #default='./mongod',
                      help='Path to mongod to run (default "./mongod")')
    parser.add_option('--port', dest='mongodPort', default="32000",
                      help='Port the mongod will bind to (default 32000)')
    parser.add_option('--mongo', dest='shellExecutable', #default="./mongo",
                      help='Path to mongo, for .js test files (default "./mongo")')
    parser.add_option('--continue-on-failure', dest='continueOnFailure',
                      action="store_true", default=False,
                      help='If supplied, continue testing even after a test fails')
    parser.add_option('--one-mongod-per-test', dest='oneMongodPerTest',
                      action="store_true", default=False,
                      help='If supplied, run each test in a fresh mongod')
    parser.add_option('--from-file', dest='File',
                      help="Run tests/suites named in FILE, one test per line, '-' means stdin")
    parser.add_option('--smoke-db-prefix', dest='smokeDbPrefix', default='',
                      help="Prefix to use for the mongods' dbpaths.")
    parser.add_option('--small-oplog', dest='smallOplog', default=False,
                      action="store_true",
                      help='Run tests with master/slave replication & use a small oplog')
    global tests
    (options, tests) = parser.parse_args()

#    global mongoRepo
#    if options.mongoRepo:
#        pass
#        mongoRepo = options.mongoRepo
#    else:
#        prefix = ''
#        while True:
#            if os.path.exists(prefix+'buildscripts'):
#                mongoRepo = os.path.normpath(prefix)
#                break
#            else:
#                prefix += '../'
#                # FIXME: will this be a device's root directory on
#                # Windows?
#                if os.path.samefile('/', prefix): 
#                    raise Exception("couldn't guess the mongo repository path")

    print tests

    global mongoRepo, mongodExecutable, mongodPort, shellExecutable, continueOnFailure, oneMongodPerTest, smallOplog, smokeDbPrefix, testPath
    testPath = options.testPath
    mongodExecutable = options.mongodExecutable if options.mongodExecutable else os.path.join(mongoRepo, 'mongod')
    mongodPort = options.mongodPort if options.mongodPort else mongodPort
    shellExecutable = options.shellExecutable if options.shellExecutable else os.path.join(mongoRepo, 'mongo')
    continueOnFailure = options.continueOnFailure if options.continueOnFailure else continueOnFailure
    oneMongodPerTest = options.oneMongodPerTest if options.oneMongodPerTest else oneMongodPerTest
    smokeDbPrefix = options.smokeDbPrefix
    smallOplog = options.smallOplog
    
    if options.File:
        if options.File == '-':
            tests = sys.stdin.readlines()
        else:
            with open(options.File) as f:
                tests = f.readlines()
    tests = [t.rstrip('\n') for t in tests]

    if not tests:
        raise Exception( "no tests specified" )
    # If we're in suite mode, tests is a list of names of sets of tests.
    if options.mode == 'suite':
        # Suites: smoke, smokePerf, smokeJs, smokeQuota, smokeJsPerf,
        # smokeJsSlow, smokeParalell, smokeClone, smokeRepl, smokeDisk
        suites = tests
        tests = []
        expandSuites(suites)
    elif options.mode == 'files':
        tests = [(os.path.abspath(test), True) for test in tests]

    runTests(tests)
    global exit_bad
    exit_bad = False

atexit.register(report)

if __name__ == "__main__":
    main()
