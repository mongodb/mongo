
import re

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
