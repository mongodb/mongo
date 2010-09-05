
import re
import socket
import time
import os
# various utilities that are handy

def getAllSourceFiles( arr=None , prefix="." ):
    if arr is None:
        arr = []

    for x in os.listdir( prefix ):
        if x.startswith( "." ) or x.startswith( "pcre-" ) or x.startswith( "32bit" ) or x.startswith( "mongodb-" ) or x.startswith("debian") or x.startswith( "mongo-cxx-driver" ):
            continue
        full = prefix + "/" + x
        if os.path.isdir( full ) and not os.path.islink( full ):
            getAllSourceFiles( arr , full )
        else:
            if full.endswith( ".cpp" ) or full.endswith( ".h" ) or full.endswith( ".c" ):
                arr.append( full )

    return arr


def getGitBranch():
    if not os.path.exists( ".git" ):
        return None

    version = open( ".git/HEAD" ,'r' ).read().strip()
    if not version.startswith( "ref: " ):
        return version
    version = version.split( "/" )
    version = version[len(version)-1]
    return version

def getGitBranchString( prefix="" , postfix="" ):
    t = re.compile( '[/\\\]' ).split( os.getcwd() )
    if len(t) > 2 and t[len(t)-1] == "mongo":
        par = t[len(t)-2]
        m = re.compile( ".*_([vV]\d+\.\d+)$" ).match( par )
        if m is not None:
            return prefix + m.group(1).lower() + postfix
        if par.find("Nightly") > 0:
            return ""


    b = getGitBranch()
    if b == None or b == "master":
        return ""
    return prefix + b + postfix

def getGitVersion():
    if not os.path.exists( ".git" ):
        return "nogitversion"

    version = open( ".git/HEAD" ,'r' ).read().strip()
    if not version.startswith( "ref: " ):
        return version
    version = version[5:]
    f = ".git/" + version
    if not os.path.exists( f ):
        return version
    return open( f , 'r' ).read().strip()

def execsys( args ):
    import subprocess
    if isinstance( args , str ):
        r = re.compile( "\s+" )
        args = r.split( args )
    p = subprocess.Popen( args , stdout=subprocess.PIPE , stderr=subprocess.PIPE )
    r = p.communicate()
    return r;

def getprocesslist():
    raw = ""
    try:
        raw = execsys( "/bin/ps -ax" )[0]
    except Exception,e:
        print( "can't get processlist: " + str( e ) )

    r = re.compile( "[\r\n]+" )
    return r.split( raw )

def removeIfInList( lst , thing ):
    if thing in lst:
        lst.remove( thing )

def findVersion( root , choices ):
    for c in choices:
        if ( os.path.exists( root + c ) ):
            return root + c
    raise "can't find a version of [" + root + "] choices: " + choices

def choosePathExist( choices , default=None):
    for c in choices:
        if c != None and os.path.exists( c ):
            return c
    return default

def filterExists(paths):
    return filter(os.path.exists, paths)

def ensureDir( name ):
    d = os.path.dirname( name )
    if not os.path.exists( d ):
        print( "Creating dir: " + name );
        os.makedirs( d )
        if not os.path.exists( d ):
            raise "Failed to create dir: " + name


def distinctAsString( arr ):
    s = set()
    for x in arr:
        s.add( str(x) )
    return list(s)

def checkMongoPort( port=27017 ):
    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(1)
    sock.connect(("localhost", port))
    sock.close()


def didMongodStart( port=27017 , timeout=20 ):
    while timeout > 0:
        time.sleep( 1 )
        try:
            checkMongoPort( port )
            return True
        except Exception,e:
            print( e )
            timeout = timeout - 1
    return False

