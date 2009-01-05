
# build file for 10gen db
# this request scons
# you can get from http://www.scons.org
# then just type scons

import os

env = Environment()

env.Append( CPPFLAGS="-fPIC -fno-strict-aliasing -ggdb -pthread -O3 -Wall -Wsign-compare -Wno-non-virtual-dtor" )
env.Append( CPPPATH=[ "." ] )
env.Append( LIBS=[ "pcrecpp" , "pcre" , "stdc++" ] )


boostLibs = [ "thread" , "filesystem" , "program_options" ]

commonFiles = Split( "stdafx.cpp db/jsobj.cpp db/json.cpp db/commands.cpp db/lasterror.cpp " ) + Glob( "util/*.cpp" ) + Glob( "grid/*.cpp" ) + Glob( "client/*.cpp" ) 

coreDbFiles = Split( "db/query.cpp db/introspect.cpp db/btree.cpp db/clientcursor.cpp db/javajs.cpp db/tests.cpp db/repl.cpp db/btreecursor.cpp db/cloner.cpp db/namespace.cpp db/matcher.cpp db/dbcommands.cpp db/dbeval.cpp db/dbwebserver.cpp db/dbinfo.cpp db/dbhelpers.cpp db/instance.cpp db/pdfile.cpp" )

if "darwin" == os.sys.platform:
    env.Append( CPPPATH=[ "/sw/include" , "-I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/" ] )
    env.Append( LIBPATH=["/sw/lib/"] )

    env.Append( CPPFLAGS=" -mmacosx-version-min=10.4 " )
    env.Append( FRAMEWORKS=["JavaVM"] )

    if os.path.exists( "/usr/bin/g++-4.2" ):
        env["CXX"] = "g++-4.2"
    

for b in boostLibs:
    env.Append( LIBS=[ "boost_" + b ] )


env.Program( "mongodump" , commonFiles + coreDbFiles + [ "tools/dump.cpp" ] )
env.Program( "db/db" , commonFiles + coreDbFiles + [ "db/db.cpp" ] )

env.Program( "db/dbgrid" , commonFiles + Glob( "dbgrid/*.cpp" ) )
