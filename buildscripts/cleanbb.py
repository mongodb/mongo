
import sys
import os
import utils
import time

cwd = os.getcwd();
if cwd.find("buildscripts" ) > 0 :
    cwd = cwd.partition( "buildscripts" )[0]

print( "cwd [" + cwd + "]" )

def shouldKill( c ):
    if c.find( cwd ) >= 0:
        return True

    if c.find( "buildbot" ) >= 0 and c.find( "/mongo/" ) >= 0:
        return True

    return False

def killprocs( signal="" ):

    killed = 0
        
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
        
        pid = x.partition( " " )[0]
        print( "killing: " + x )
        utils.execsys( "/bin/kill " + signal + " " +  pid )
        killed = killed + 1

    return killed


def cleanup( root ):
    

    if killprocs() > 0:
        time.sleep(3)
        killprocs("-9")

    # delete all regular files, directories can stay
    # NOTE: if we delete directories later, we can't delete diskfulltest
    for ( dirpath , dirnames , filenames ) in os.walk( root , topdown=False ):
        for x in filenames: 
            foo = dirpath + "/" + x
            print( "removing: " + foo )
            os.remove( foo )


if __name__ == "__main__":
    root = "/data/db/"
    if len( sys.argv ) > 1:
        root = sys.argv[1]
    cleanup( root )
