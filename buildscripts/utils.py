
import re
import socket
import time
import os
# various utilities that are handy

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

