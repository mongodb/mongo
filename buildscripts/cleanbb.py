
import sys
import os, os.path
import utils
import time
import exceptions
from optparse import OptionParser

# set cwd to the root mongo dir, one level up from this
# file's location (if we're not already running from there)
cwd = os.getcwd()
if os.path.basename(cwd) == 'buildscripts':
    cwd = os.path.dirname(cwd)

print( "cwd [" + cwd + "]" )

def shouldKill( c ):
    if "smoke.py" in c:
        return False

    if "emr.py" in c:
        return False

    if "java" in c:
        return False

    if c.find( cwd ) >= 0:
        return True

    if ( c.find( "buildbot" ) >= 0 or c.find( "slave" ) >= 0 ) and c.find( "/mongo/" ) >= 0:
        return True

    if c.find( "xml-data/build-dir" ) >= 0: # for bamboo
        return True

    return False

def killprocs( signal="" ):
    killed = 0

    if sys.platform == 'win32':
        return killed

    l = utils.getprocesslist()
    print( "num procs:" + str( len( l ) ) )
    if len(l) == 0:
        print( "no procs" )
        try:
            print( execsys( "/sbin/ifconfig -a" ) )
        except Exception,e:
            print( "can't get interfaces" + str( e ) )

    for x in l:
        x = x.lstrip()
        if not shouldKill( x ):
            continue

        pid = x.split( " " )[0]
        print( "killing: " + x )
        utils.execsys( "/bin/kill " + signal + " " +  pid )
        killed = killed + 1

    return killed


def cleanup( root , nokill ):
    if nokill:
        print "nokill requested, not killing anybody"
    else:
        if killprocs() > 0:
            time.sleep(3)
            killprocs("-9")

    # delete all regular files, directories can stay
    # NOTE: if we delete directories later, we can't delete diskfulltest
    for ( dirpath , dirnames , filenames ) in os.walk( root , topdown=False ):
        for x in filenames:
            foo = dirpath + "/" + x
            print( "removing: " + foo )
            try:
                os.remove(foo)
            except exceptions.OSError, e:
                # SERVER-10462 compensate for Windows file locking race
                # We want to catch WindowsError but can't use that name on other platforms
                print(repr(e))
                print("os.remove(%s) failed, retrying once." % foo)
                time.sleep(1)
                os.remove(foo)

if __name__ == "__main__":
    parser = OptionParser(usage="read the script")
    parser.add_option("--nokill", dest='nokill', default=False, action='store_true')
    (options, args) = parser.parse_args()

    root = "/data/db/"
    if len(args) > 0:
        root = args[0]

    cleanup( root , options.nokill )
