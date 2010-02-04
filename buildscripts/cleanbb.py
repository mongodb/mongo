
import sys
import os
import utils
import time

def killprocs( signal="" ):
    cwd = os.getcwd();
    if cwd.find("buildscripts" ) > 0 :
        cwd = cwd.partition( "buildscripts" )[0]

    killed = 0
        
    for x in utils.getprocesslist():
        x = x.lstrip()
        if x.find( cwd ) < 0:
            continue
        
        pid = x.partition( " " )[0]
        print( "killing: " + x )
        utils.execsys( "/bin/kill " + signal + " " +  pid )
        killed = killed + 1

    return killed


def cleanup( root ):
    
    # delete all regular files, directories can stay
    # NOTE: if we delete directories later, we can't delete diskfulltest
    for ( dirpath , dirnames , filenames ) in os.walk( root , topdown=False ):
        for x in filenames: 
            os.remove( dirpath + "/" + x )

    if killprocs() > 0:
        time.sleep(3)
        killprocs("-9")

if __name__ == "__main__":
    root = "/data/db/"
    if len( sys.argv ) > 1:
        root = sys.argv[1]
    cleanup( root )
