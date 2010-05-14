#!/usr/bin/python

from subprocess import Popen, PIPE, call
import os
import sys
import utils
import time
import socket
from optparse import OptionParser
import atexit

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
        print "FOO"
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

def runTest(path):
    (ignore, ext) = os.path.splitext(path)
    if ext == ".js":
        argv=[shellExecutable, "--port", mongodPort, path]
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
    parser = OptionParser(usage="usage: smoke.py [OPTIONS] FILE*")
    parser.add_option('--mongod', dest='mongodExecutable', default='./mongod',
                      help='Path to mongod to run (default "./mongod")')
    parser.add_option('--port', dest='mongodPort', default="32000",
                      help='Port the mongod will bind to (default 32000)')
    parser.add_option('--mongo', dest='shellExecutable', default="./mongo",
                      help='Path to mongo, for .js test files (default "./mongo")')
    parser.add_option('--continue-on-failure', dest='continueOnFailure',
                      action="store_true", default=False,
                      help='If supplied, continue testing even after a failure')
    parser.add_option('--one-mongod-per-test', dest='oneMongodPerTest',
                      action="store_true", default=False,
                      help='If supplied, run each test in a fresh mongod')
    parser.add_option('--from-file', dest='File',
                      help="Run tests named in FILE, one test per line, '-' means stdin")
    (options, tests) = parser.parse_args()
    

    global mongodExecutable, mongodPort, shellExecutable, continueOnFailure, oneMongodPerTest
    mongodExecutable = options.mongodExecutable if options.mongodExecutable else mongodExecutable
    mongodPort = options.mongodPort if options.mongodPort else mongodPort
    shellExecutable = options.shellExecutable if options.shellExecutable else shellExecutable
    continueOnFailure = options.continueOnFailure if options.continueOnFailure else continueOnFailure
    oneMongodPerTest = options.oneMongodPerTest if options.oneMongodPerTest else oneMongodPerTest
    
    global tests
    if options.File:
        if options.File == '-':
            tests = sys.stdin.readlines()
        else:
            with open(options.File) as f:
                tests = f.readlines()
    tests = [t.rstrip('\n') for t in tests]

    return runTests(tests)

atexit.register(report)

if __name__ == "__main__":
    main()
