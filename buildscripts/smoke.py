#!/usr/bin/python

from subprocess import Popen, PIPE, call
import os
import sys
import utils
import time
import socket
from optparse import OptionParser
import atexit
import glob

mongodExecutable = "./mongod"
mongodPort = "32000"
shellExecutable = "./mongo"
continueOnFailure = False
oneMongodPerTest = False

tests = []
winners = []
losers = {}
# grumble
class nothing(object):
    def __enter__(self):
        return self
    def __exit__(self, type, value, traceback):
        return not isinstance(value, Exception)

class mongod(object):
    def __init__(self, *args):
        self.args = args
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
        utils.ensureDir( "/tmp/unittest/" )
        utils.ensureDir( "/data/" )
        utils.ensureDir( "/data/db/" )

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
                print( e )
                timeout = timeout - 1
        return False

    def start(self):
        global mongodPort
        global mongod
        if self.proc:
            print >> sys.stderr, "probable bug: self.proc already set in start()"
            return
        self.ensureTestDirs()
        dirName = "/data/db/sconsTests/"
        utils.ensureDir( dirName )
        argv = [mongodExecutable, "--port", str(mongodPort),
                "--dbpath", dirName] + list(self.args)
        print argv
        self.proc = Popen(argv)
        if not self.didMongodStart( mongodPort ):
            raise Exception( "Failed to start mongod" )

#    def startMongodSmallOplog(env, target, source):
#        return startMongodWithArgs("--master", "--oplogSize", "10")
    
    def stop(self):
        if not self.proc:
            print >> sys.stderr, "probable bug: self.proc unset in stop()"
            return
        # ???
        #if self.proc.poll() is not None:
        #    raise Exception( "Failed to start mongod" )
        try:
            # This function not available in Python 2.5
            self.proc.terminate()
        except AttributeError:
            if os.sys.platform == "windows":
                import win32process
                win32process.TerminateProcess(self.proc._handle, -1)
            else:
                from os import kill
                kill( self.proc.pid, 15 )
        self.proc.wait()
        sys.stderr.flush()
        sys.stdout.flush()
    
class Bug(Exception):
    pass

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
    def __str__(self):
        return 'mongod not ok after test %s' % self.path

def runTest(test):
    (path, usedb) = test
    (ignore, ext) = os.path.splitext(path)
    if ext == ".js":
        argv=filter(lambda x:x, [shellExecutable, "--port", mongodPort] + [None if usedb else "--nodb"] + [path])
    elif ext in ["", ".exe"]:
        argv=[path, "--port", mongodPort]
    else:
        raise Bug("fell off in extenstion case: %s" % path)
    print " *******************************************"
    print "         Test : " + os.path.basename(path) + " ..."
    t1=time.time()
    # FIXME: we don't handle the case where the subprocess
    # hangs... that's bad.
    r = call(argv)
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
    with nothing() if oneMongodPerTest else mongod() as nevermind:
        for test in tests:
            try:
                with mongod() if oneMongodPerTest else nothing() as nevermind:
                    runTest(test)
                winners.append(test)
            except TestFailure, f:
                try:
                    print f
                    losers[f.path] = f.status
                    raise f
                # If the server's hosed and we're not in oneMongodPerTest
                # mode, there's nothing else we can do.
                except TestServerFailure, f: 
                    if not oneMongodPerTest:
                        return 2
                except TestFailure, f:
                    if not continueOnFailure:
                        return 1
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

def main():
    parser = OptionParser(usage="usage: smoke.py [OPTIONS] ARGS*")
    parser.add_option('--mode', dest='mode', default='files',
                      help='If "files", ARGS are filenames; if "suite", ARGS are sets of tests.  (default "files")')
    parser.add_option('--mongo-repo', dest='mongoRepo', default=None,
                      help='Top-level directory of mongo checkout to use.  (default: script will make a guess)')
    parser.add_option('--mongod', dest='mongodExecutable', default='./mongod',
                      help='Path to mongod to run (default "./mongod" in the mongo repo)')
    parser.add_option('--port', dest='mongodPort', default="32000",
                      help='Port the mongod will bind to (default 32000)')
    parser.add_option('--mongo', dest='shellExecutable', default="./mongo",
                      help='Path to mongo, for .js test files (default "./mongo" in the mongo repo)')
    parser.add_option('--continue-on-failure', dest='continueOnFailure',
                      action="store_true", default=False,
                      help='If supplied, continue testing even after a test fails')
    parser.add_option('--one-mongod-per-test', dest='oneMongodPerTest',
                      action="store_true", default=False,
                      help='If supplied, run each test in a fresh mongod')
    parser.add_option('--from-file', dest='File',
                      help="Run tests/suites named in FILE, one test per line, '-' means stdin")
    global tests
    (options, tests) = parser.parse_args()

    if options.mongoRepo:
        mongoRepo = options.mongoRepo
    else:
        prefix = ''
        while True:
            if os.stat(prefix+'buildscripts'):
                mongoRepo = os.path.normpath(prefix)
                break
            else:
                prefix += '../'
                # FIXME: will this be a device's root directory on
                # Windows?
                if os.path.samefile('/', prefix): 
                    raise Exception("couldn't guess the mongo repository path")

    global mongodExecutable, mongodPort, shellExecutable, continueOnFailure, oneMongodPerTest
    mongodExecutable = options.mongodExecutable if options.mongodExecutable else os.path.join(mongoRepo, mongodExecutable)
    mongodPort = options.mongodPort if options.mongodPort else mongodPort
    shellExecutable = options.shellExecutable if options.shellExecutable else os.path.join(mmongoRepo, shellExecutable)
    continueOnFailure = options.continueOnFailure if options.continueOnFailure else continueOnFailure
    oneMongodPerTest = options.oneMongodPerTest if options.oneMongodPerTest else oneMongodPerTest
    
    if options.File:
        if options.File == '-':
            tests = sys.stdin.readlines()
        else:
            with open(options.File) as f:
                tests = f.readlines()
    tests = [t.rstrip('\n') for t in tests]

    # If we're in suite mode, tests is a list of names of sets of tests.
    if options.mode == 'suite':
        # Suites: smoke, smokePerf, smokeJs, smokeQuota, smokeJsPerf,
        # smokeJsSlow, smokeParalell, smokeClone, smokeRepl, smokeDisk
        suites = tests
        tests = []
        for suite in suites:
            if suite == 'smoke':
                (globstr, usedb) = ('test', False)
            elif suite == 'smokePerf':
                (globstr, usedb) = ('perftest', False)
            elif suite == 'smokeClient':
                (globstr, usedb) = ('*Test', False)
                # (globstr, usedb) = ('*Example', False)
            elif suite == 'smokeJs':
                # FIXME: _runner.js seems equivalent to "[!_]*.js".
                #(globstr, usedb) = ('_runner.js', True)
                (globstr, usedb) = ('[!_]*.js', True)
            elif suite == 'smokeQuota':
                (globstr, usedb) = ('quota/*.js', True)
            elif suite == 'smokeJsPerf':
                (globstr, usedb) = ('perf/*.js', True)
            elif suite == 'smokeJsSlow':
                (globstr, usedb) = ('slow/*.js', True)
            elif suite == 'smokeParallel':
                (globstr, usedb) = ('parallel/*', True)
            elif suite == 'smokeClone':
                (globstr, usedb) = ('clone/*.js', False)
            elif suite == 'smokeRepl':
                (globstr, usedb) = ('repl/*.js', False)
            elif suite == 'smokeAuth':
                (globstr, usedb) = ('auth/*.js', False)
            elif suite == 'smokeSharding':
                (globstr, usedb) = ('sharding/*.js', False)
            elif suite == 'smokeTool':
                (globstr, usedb) = ('tool/*.js', False)
            else:
                raise Exception('unknown test suite %s' % suite)

            globstr = mongoRepo+('/jstests/' if globstr.endswith('.js') else '/')+globstr
            print globstr
            print glob.glob(globstr)
            tests += [(path, usedb) for path in glob.glob(globstr)]
    elif options.mode == 'files':
        tests = [(os.path.abspath(test), True) for test in tests]

    print tests
    return runTests(tests)

atexit.register(report)

if __name__ == "__main__":
    main()
