
import os

def insert( env , options ):

    # now that sm is in the source tree, don't need this
    # if not foundxulrunner( env , options ):
    #     if os.path.exists( "usr/include/mozjs/" ):
    #         env.Append( CPPDEFINES=[ "MOZJS" ] )

    return

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
        print( "warning: using ubuntu without xulrunner-dev.  we recommend installing it" )
        return False

    incroot = "/usr/include/" + best + "/"
    libroot = "/usr/lib"
    if options["linux64"] and os.path.exists("/usr/lib64"):
        libroot += "64";
    libroot += "/" + best

    
    if not os.path.exists( libroot ):
        print( "warning: found xulrunner include but not lib for: " + best )
        return False
    
    env.Prepend( LIBPATH=[ libroot ] )
    env.Prepend( RPATH=[ libroot ] )

    env.Prepend( CPPPATH=[ incroot + "stable/" , 
                           incroot + "unstable/" ,
                           incroot ] )
    env.Prepend( CPPPATH=[ "/usr/include/nspr/" ] )
    
    env.Append( CPPDEFINES=[ "XULRUNNER" , "OLDJS" ] )
    if best.find( "1.9.0" ) >= 0 or best.endswith("1.9"):
        if best.endswith( "1.9.1.9" ):
            pass
        else:
            env.Append( CPPDEFINES=[ "XULRUNNER190" ] )
    return True
