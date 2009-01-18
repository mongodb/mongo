
# build file for 10gen db
# this request scons
# you can get from http://www.scons.org
# then just type scons

import os

# --- options ----
AddOption('--prefix',
          dest='prefix',
          type='string',
          nargs=1,
          action='store',
          metavar='DIR',
          help='installation prefix')


# --- environment setup ---

env = Environment()

env.Append( CPPPATH=[ "." ] )


boostLibs = [ "thread" , "filesystem" , "program_options" ]

commonFiles = Split( "stdafx.cpp db/jsobj.cpp db/json.cpp db/commands.cpp db/lasterror.cpp db/security.cpp " )
commonFiles += Glob( "util/*.cpp" ) + Glob( "util/*.c" ) + Glob( "grid/*.cpp" )
commonFiles += Split( "client/connpool.cpp client/dbclient.cpp client/model.cpp" ) 

coreDbFiles = Split( "" )

serverOnlyFiles = Split( "db/query.cpp db/introspect.cpp db/btree.cpp db/clientcursor.cpp db/javajs.cpp db/tests.cpp db/repl.cpp db/btreecursor.cpp db/cloner.cpp db/namespace.cpp db/matcher.cpp db/dbcommands.cpp db/dbeval.cpp db/dbwebserver.cpp db/dbinfo.cpp db/dbhelpers.cpp db/instance.cpp db/pdfile.cpp db/cursor.cpp" )

allClientFiles = commonFiles + coreDbFiles + [ "client/clientOnly.cpp" ];

nix = False

def findVersion( root , choices ):
    for c in choices:
        if ( os.path.exists( root + c ) ):
            return root + c
    raise "can't find a version of [" + root + "] choices: " + choices

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
    javaHome = findVersion( "C:/Program Files/java/" , 
                            [ "jdk" , "jdk1.6.0_10" ] )
    winSDKHome = findVersion( "C:/Program Files/Microsoft SDKs/Windows/" , 
                              [ "v6.0" , "v6.0a" , "v6.1" ] )

    env.Append( CPPPATH=[ boostDir , javaHome + "/include" , javaHome + "/include/win32" , "pcre-7.4" , winSDKHome + "/Include" ] )

    # /Fo"Debug\\" /Fd"Debug\vc90.pdb" 

    env.Append( CPPFLAGS=" /Od /EHsc /Gm /RTC1 /MDd /ZI  /W3 " )
    env.Append( CPPDEFINES=["WIN32","_DEBUG","_CONSOLE","_CRT_SECURE_NO_WARNINGS","HAVE_CONFIG_H","PCRE_STATIC","_UNICODE","UNICODE" ] )

    env.Append( LIBPATH=[ boostDir + "/Lib" , javaHome + "/Lib" , winSDKHome + "/Lib" ] )
    env.Append( LIBS=[ "jvm" ] )

    def pcreFilter(x):
        name = x.name
        if x.name.endswith( "dftables.c" ):
            return False
        if x.name.endswith( "pcredemo.c" ):
            return False
        if x.name.endswith( "pcretest.c" ):
            return False
        if x.name.endswith( "unittest.cc" ):
            return False
        if x.name.endswith( "pcregrep.c" ):
            return False
        return True

    commonFiles += filter( pcreFilter , Glob( "pcre-7.4/*.c"  ) )
    commonFiles += filter( pcreFilter , Glob( "pcre-7.4/*.cc" ) )
    
    env.Append( LIBS=Split("ws2_32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib" ) )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )


if nix:
    env.Append( CPPFLAGS="-fPIC -fno-strict-aliasing -ggdb -pthread -O3 -Wall -Wsign-compare -Wno-non-virtual-dtor" )
    env.Append( LIBS=[ "pcrecpp" , "pcre" , "stdc++" ] )

    for b in boostLibs:
        env.Append( LIBS=[ "boost_" + b ] )

clientEnv = env.Clone();
clientEnv.Append( CPPPATH=["../"] )
clientEnv.Append( LIBS=[ "libmongoclient.a"] )
clientEnv.Append( LIBPATH=["."] )

testEnv = env.Clone()
testEnv.Append( CPPPATH=["../"] )
testEnv.Append( LIBS=[ "unittest" , "libmongotestfiles.a" ] )
testEnv.Append( LIBPATH=["."] )

# SYSTEM CHECKS
configure = env.Configure()



# ----- TARGETS ------


# main db target
Default( env.Program( "db/db" , commonFiles + coreDbFiles + serverOnlyFiles + [ "db/db.cpp" ]  ) )

# tools
env.Program( "mongodump" , allClientFiles + [ "tools/dump.cpp" ] )
env.Program( "mongoimport" , allClientFiles + [ "tools/import.cpp" ] )

# dbgrid
env.Program( "db/dbgrid" , commonFiles + coreDbFiles + Glob( "dbgrid/*.cpp" ) )

# c++ library
env.Library( "mongoclient" , allClientFiles )
env.Library( "mongotestfiles" , commonFiles + coreDbFiles + serverOnlyFiles )

# examples
clientEnv.Program( "firstExample" , [ "client/examples/first.cpp" ] )
clientEnv.Program( "secondExample" , [ "client/examples/second.cpp" ] )

# testing
test = testEnv.Program( "test" , Glob( "dbtests/*.cpp" ) )
clientEnv.Program( "clientTest" , [ "client/examples/clientTest.cpp" ] )

#  ---- RUNNING TESTS ----

testEnv.Alias( "smoke", "test", test[ 0 ].abspath )
testEnv.AlwaysBuild( "smoke" )

#  ----  INSTALL -------

installDir = "/usr/local"
if GetOption( "prefix" ):
    installDir = GetOption( "prefix" )

#binaries
env.Install( installDir + "/bin" , "mongodump" )
env.Install( installDir + "/bin" , "mongoimport" )
env.Install( installDir + "/bin" , "db/db" )

#headers
for id in [ "" , "client/" , "util/" , "grid/" , "db/" ]:
    env.Install( installDir + "/include/mongo/" + id , Glob( id + "*.h" ) )

#lib
env.Install( installDir + "/lib" , "libmongoclient.a" )
env.Install( installDir + "/lib/mongo-jars" , Glob( "jars/*" ) )

#final alias
env.Alias( "install" , installDir )

#  ---- CONVENIENCE ----

def tabs( env, target, source ):
    from subprocess import Popen, PIPE
    from re import search, match
    diff = Popen( [ "git", "diff", "-U0", "origin", "master" ], stdout=PIPE ).communicate()[ 0 ]
    sourceFile = False
    for line in diff.split( "\n" ):
        if match( "diff --git", line ):
            sourceFile = not not search( "\.(h|hpp|c|cpp)\s*$", line )
        if sourceFile and match( "\+ *\t", line ):
            return True
    return False
env.Alias( "checkSource", [], [ tabs ] )
env.AlwaysBuild( "checkSource" )

def gitPush( env, target, source ):
    import subprocess
    return subprocess.call( [ "git", "push" ] )
env.Alias( "push", [ ".", "smoke", "checkSource" ], gitPush )
env.AlwaysBuild( "push" )
