
# build file for 10gen db
# this request scons
# you can get from http://www.scons.org
# then just type scons

import os

env = Environment()

env.Append( CPPPATH=[ "." ] )


boostLibs = [ "thread" , "filesystem" , "program_options" ]

commonFiles = Split( "stdafx.cpp db/jsobj.cpp db/json.cpp db/commands.cpp db/lasterror.cpp " ) + Glob( "util/*.cpp" ) + Glob( "grid/*.cpp" ) + Glob( "client/*.cpp" ) 

coreDbFiles = Split( "db/query.cpp db/introspect.cpp db/btree.cpp db/clientcursor.cpp db/javajs.cpp db/tests.cpp db/repl.cpp db/btreecursor.cpp db/cloner.cpp db/namespace.cpp db/matcher.cpp db/dbcommands.cpp db/dbeval.cpp db/dbwebserver.cpp db/dbinfo.cpp db/dbhelpers.cpp db/instance.cpp db/pdfile.cpp" )

nix = False

if "darwin" == os.sys.platform:
    env.Append( CPPPATH=[ "/sw/include" , "-I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/" ] )
    env.Append( LIBPATH=["/sw/lib/"] )

    env.Append( CPPFLAGS=" -mmacosx-version-min=10.4 " )
    env.Append( FRAMEWORKS=["JavaVM"] )

    if os.path.exists( "/usr/bin/g++-4.2" ):
        env["CXX"] = "g++-4.2"

    nix = True

elif "linux2" == os.sys.platform:
    javaHome = "/opt/java/"

    env.Append( CPPPATH=[ javaHome + "include" , javaHome + "include/linux"] )
    
    javaVersion = "i386";

    if os.uname()[4] == "x86_64":
        javaVersion = "amd64"

    env.Append( LIBPATH=[ javaHome + "jre/lib/" + javaVersion + "/server" , javaHome + "jre/lib/" + javaVersion ] )
    env.Append( LIBS=[ "java" , "jvm" ] )

    env.Append( LINKFLAGS="-Xlinker -rpath -Xlinker " + javaHome + "jre/lib/" + javaVersion + "/server" )
    env.Append( LINKFLAGS="-Xlinker -rpath -Xlinker " + javaHome + "jre/lib/" + javaVersion  )

    nix = True

elif "win32" == os.sys.platform:
    boostDir = "C:/Program Files/Boost/boost_1_35_0"
    javaHome = "C:/Program Files/java/jdk/"
    winSDKHome = "C:/Program Files/Microsoft SDKs/Windows/v6.0"

    env.Append( CPPPATH=[ boostDir , javaHome + "include" , javaHome + "include/win32" , "pcre-7.4" , winSDKHome + "/Include" ] )

    # /Fo"Debug\\" /Fd"Debug\vc90.pdb" 

    env.Append( CPPFLAGS=" /Od /EHsc /Gm /RTC1 /MDd /ZI  /W3 " )
    env.Append( CPPDEFINES=["WIN32","_DEBUG","_CONSOLE","_CRT_SECURE_NO_WARNINGS","HAVE_CONFIG_H","PCRE_STATIC","_UNICODE","UNICODE" ] )

    env.Append( LIBPATH=[ boostDir + "/Lib" , javaHome + "Lib" , winSDKHome + "/Lib" ] )

    commonFiles += Glob( "pcre-7.4/*.c"  )
    print( commonFiles )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )


if nix:
    env.Append( CPPFLAGS="-fPIC -fno-strict-aliasing -ggdb -pthread -O3 -Wall -Wsign-compare -Wno-non-virtual-dtor" )
    env.Append( LIBS=[ "pcrecpp" , "pcre" , "stdc++" ] )

    for b in boostLibs:
        env.Append( LIBS=[ "boost_" + b ] )


env.Program( "mongodump" , commonFiles + coreDbFiles + [ "tools/dump.cpp" ] )
Default( env.Program( "db/db" , commonFiles + coreDbFiles + [ "db/db.cpp" ]  ) )

env.Program( "db/dbgrid" , commonFiles + Glob( "dbgrid/*.cpp" ) )
