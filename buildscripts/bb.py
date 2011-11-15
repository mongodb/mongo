# bb tools

import os
import re

def checkOk():
    dir = os.getcwd()
    m = re.compile( ".*/.*_V(\d+\.\d+)/mongo" ).findall( dir )
    if len(m) == 0:
        return
    if len(m) > 1:
        raise Exception( "unexpected: " + str(m) )
    
    m = "v" + m[0]
    print( m )
    print( "expected version [" + m + "]" )

    from subprocess import Popen, PIPE
    diff = Popen( [ "git", "diff", "origin/v1.2" ], stdout=PIPE ).communicate()[ 0 ]
    if len(diff) > 0:
        print( diff )
        raise Exception( "build bot broken?" )

