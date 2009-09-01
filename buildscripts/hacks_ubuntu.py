
import os

def insert( env , options ):
    
    if not foundxulrunner( env , options ):
        env.Append( CPPDEFINES=[ "MOZJS" ] )


def foundxulrunner( env , options ):
    best = None

    for x in os.listdir( "/usr/include" ):
        if x.find( "xulrunner" ) != 0:
            continue
        if x == "xulrunner":
            best = x
            break
        best = x
        

    if best is None:
        print( "warning: using ubuntu without xulrunner-dev.  we reccomend installing it" )
        return False

    incroot = "/usr/include/" + best + "/"
    libroot = "/usr/lib"
    if options["linux64"]:
        libroot += "64";
    libroot += "/" + best
    
    if not os.path.exists( libroot ):
        print( "warning: found xulrunner include but not lib for: " + best )
        return False
    
    env.Prepend( LIBPATH=[ libroot ] )
    env.Prepend( RPATH=[ libroot ] )

    env.Prepend( CPPPATH=[ incroot + "stable/" , 
                           incroot + "unstable/" ] )
    
    env.Append( CPPDEFINES=[ "XULRUNNER" ] )
    return True
