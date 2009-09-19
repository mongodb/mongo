# -*- mode: python; -*-
# build file for 10gen db
# this request scons
# you can get from http://www.scons.org
# then just type scons

# some common tasks
#   build 64-bit mac and pushing to s3
#      scons --64 s3dist
#      scons --distname=0.8 s3dist
#      all s3 pushes require settings.py and simples3

import os
import sys
import types
import re
import shutil
import urllib
import urllib2
import buildscripts

# --- options ----
AddOption('--prefix',
          dest='prefix',
          type='string',
          nargs=1,
          action='store',
          metavar='DIR',
          help='installation prefix')

AddOption('--distname',
          dest='distname',
          type='string',
          nargs=1,
          action='store',
          metavar='DIR',
          help='dist name (0.8.0)')


AddOption( "--64",
           dest="force64",
           type="string",
           nargs=0,
           action="store",
           help="whether to force 64 bit" )


AddOption( "--32",
           dest="force32",
           type="string",
           nargs=0,
           action="store",
           help="whether to force 32 bit" )


AddOption( "--mm",
           dest="mm",
           type="string",
           nargs=0,
           action="store",
           help="use main memory instead of memory mapped files" )


AddOption( "--release",
           dest="release",
           type="string",
           nargs=0,
           action="store",
           help="relase build")


AddOption( "--static",
           dest="static",
           type="string",
           nargs=0,
           action="store",
           help="fully static build")


AddOption('--java',
          dest='javaHome',
          type='string',
          default="/opt/java/",
          nargs=1,
          action='store',
          metavar='DIR',
          help='java home')

AddOption('--nojni',
          dest='nojni',
          type="string",
          nargs=0,
          action="store",
          help="turn off jni support" )


AddOption('--usesm',
          dest='usesm',
          type="string",
          nargs=0,
          action="store",
          help="use spider monkey for javascript" )

AddOption('--usejvm',
          dest='usejvm',
          type="string",
          nargs=0,
          action="store",
          help="use java for javascript" )

AddOption( "--d",
           dest="debugBuild",
           type="string",
           nargs=0,
           action="store",
           help="debug build no optimization, etc..." )

AddOption( "--dd",
           dest="debugBuildAndLogging",
           type="string",
           nargs=0,
           action="store",
           help="debug build no optimization, additional debug logging, etc..." )

AddOption( "--recstore",
           dest="recstore",
           type="string",
           nargs=0,
           action="store",
           help="use new recstore" )

AddOption( "--noshell",
           dest="noshell",
           type="string",
           nargs=0,
           action="store",
           help="don't build shell" )

AddOption( "--extrapath",
           dest="extrapath",
           type="string",
           nargs=1,
           action="store",
           help="comma seperated list of add'l paths  (--extrapath /opt/foo/,/foo" )

AddOption( "--cxx",
           dest="cxx",
           type="string",
           nargs=1,
           action="store",
           help="compiler to use" )


AddOption( "--boost-compiler",
           dest="boostCompiler",
           type="string",
           nargs=1,
           action="store",
           help="compiler used for boost (gcc41)" )

AddOption( "--boost-version",
           dest="boostVersion",
           type="string",
           nargs=1,
           action="store",
           help="boost version for linking(1_38)" )

AddOption( "--pg",
           dest="profile",
           type="string",
           nargs=0,
           action="store" )

# --- environment setup ---

def removeIfInList( lst , thing ):
    if thing in lst:
        lst.remove( thing )

def printLocalInfo():
    import sys, SCons
    print( "scons version: " + SCons.__version__ )
    print( "python version: " + " ".join( [ `i` for i in sys.version_info ] ) )

printLocalInfo()

boostLibs = [ "thread" , "filesystem" , "program_options" ]

onlyServer = len( COMMAND_LINE_TARGETS ) == 0 or ( len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) in [ "mongod" , "mongos" , "test" ] )
nix = False
useJavaHome = False
linux = False
linux64  = False
darwin = False
windows = False
freebsd = False
solaris = False
force64 = not GetOption( "force64" ) is None
if not force64 and os.getcwd().endswith( "mongo-64" ):
    force64 = True
    print( "*** assuming you want a 64-bit build b/c of directory *** " )
msarch = None
if force64:
    msarch = "amd64"

force32 = not GetOption( "force32" ) is None
release = not GetOption( "release" ) is None
static = not GetOption( "static" ) is None

debugBuild = ( not GetOption( "debugBuild" ) is None ) or ( not GetOption( "debugBuildAndLogging" ) is None )
debugLogging = not GetOption( "debugBuildAndLogging" ) is None
noshell = not GetOption( "noshell" ) is None
nojni = not GetOption( "nojni" ) is None

usesm = not GetOption( "usesm" ) is None
usejvm = not GetOption( "usejvm" ) is None

env = Environment( MSVS_ARCH=msarch , tools = ["default", "gch"], toolpath = '.' )
if GetOption( "cxx" ) is not None:
    env["CC"] = GetOption( "cxx" )
    env["CXX"] = GetOption( "cxx" )
env["LIBPATH"] = []

if GetOption( "recstore" ) != None:
    env.Append( CPPDEFINES=[ "_RECSTORE" ] )
env.Append( CPPDEFINES=[ "_SCONS" ] )
env.Append( CPPPATH=[ "." ] )



boostCompiler = GetOption( "boostCompiler" )
if boostCompiler is None:
    boostCompiler = ""
else:
    boostCompiler = "-" + boostCompiler

boostVersion = GetOption( "boostVersion" )
if boostVersion is None:
    boostVersion = ""
else:
    boostVersion = "-" + boostVersion

if ( usesm and usejvm ):
    print( "can't say usesm and usejvm at the same time" )
    Exit(1)

if ( not ( usesm or usejvm ) ):
    usesm = True

if GetOption( "extrapath" ) is not None:
    for x in GetOption( "extrapath" ).split( "," ):
        env.Append( CPPPATH=[ x + "/include" ] )
        env.Append( LIBPATH=[ x + "/lib" ] )
    release = True

# ------    SOURCE FILE SETUP -----------

commonFiles = Split( "stdafx.cpp buildinfo.cpp db/jsobj.cpp db/json.cpp db/commands.cpp db/lasterror.cpp db/nonce.cpp db/queryutil.cpp shell/mongo.cpp" )
commonFiles += [ "util/background.cpp" , "util/mmap.cpp" ,  "util/sock.cpp" ,  "util/util.cpp" , "util/message.cpp" , "util/assert_util.cpp" , "util/httpclient.cpp" , "util/md5main.cpp" ]
commonFiles += Glob( "util/*.c" )
commonFiles += Split( "client/connpool.cpp client/dbclient.cpp client/model.cpp" )
commonFiles += [ "scripting/engine.cpp" ]

#mmap stuff

if GetOption( "mm" ) != None:
    commonFiles += [ "util/mmap_mm.cpp" ]
elif os.sys.platform == "win32":
    commonFiles += [ "util/mmap_win.cpp" ]
else:
    commonFiles += [ "util/mmap_posix.cpp" ]

if os.path.exists( "util/processinfo_" + os.sys.platform + ".cpp" ):
    commonFiles += [ "util/processinfo_" + os.sys.platform + ".cpp" ]
else:
    commonFiles += [ "util/processinfo_none.cpp" ]

coreDbFiles = []
coreServerFiles = [ "util/message_server_port.cpp" , "util/message_server_asio.cpp" ]

serverOnlyFiles = Split( "db/query.cpp db/introspect.cpp db/btree.cpp db/clientcursor.cpp db/tests.cpp db/repl.cpp db/btreecursor.cpp db/cloner.cpp db/namespace.cpp db/matcher.cpp db/dbcommands.cpp db/dbeval.cpp db/dbwebserver.cpp db/dbinfo.cpp db/dbhelpers.cpp db/instance.cpp db/pdfile.cpp db/cursor.cpp db/security_commands.cpp db/security.cpp util/miniwebserver.cpp db/storage.cpp db/reccache.cpp db/queryoptimizer.cpp db/extsort.cpp" )

if usesm:
    commonFiles += [ "scripting/engine_spidermonkey.cpp" ]
    nojni = True
elif not nojni:
    commonFiles += [ "scripting/engine_java.cpp" ]
else:
    commonFiles += [ "scripting/engine_none.cpp" ]
    nojni = True

coreShardFiles = []
shardServerFiles = coreShardFiles + Glob( "s/strategy*.cpp" ) + [ "s/commands_admin.cpp" , "s/commands_public.cpp" , "s/request.cpp" ,  "s/cursors.cpp" ,  "s/server.cpp" , "s/chunk.cpp" , "s/shardkey.cpp" , "s/config.cpp" ]
serverOnlyFiles += coreShardFiles + [ "s/d_logic.cpp" ]

allClientFiles = commonFiles + coreDbFiles + [ "client/clientOnly.cpp" , "client/gridfs.cpp" ];

allCXXFiles = allClientFiles + coreShardFiles + shardServerFiles + serverOnlyFiles;

# ---- other build setup -----

platform = os.sys.platform
if "uname" in dir(os):
    processor = os.uname()[4]
else:
    processor = "i386"

if force32:
    processor = "i386"
if force64:
    processor = "x86_64"

DEFAULT_INSTALl_DIR = "/usr/local"
installDir = DEFAULT_INSTALl_DIR
nixLibPrefix = "lib"

distName = GetOption( "distname" )
dontReplacePackage = False

javaHome = GetOption( "javaHome" )
javaVersion = "i386";
javaLibs = []

distBuild = len( COMMAND_LINE_TARGETS ) == 1 and ( str( COMMAND_LINE_TARGETS[0] ) == "s3dist" or str( COMMAND_LINE_TARGETS[0] ) == "dist" )
if distBuild:
    release = True

if GetOption( "prefix" ):
    installDir = GetOption( "prefix" )

def findVersion( root , choices ):
    for c in choices:
        if ( os.path.exists( root + c ) ):
            return root + c
    raise "can't find a version of [" + root + "] choices: " + choices

def choosePathExist( choices , default=None):
    for c in choices:
        if c != None and os.path.exists( c ):
            return c
    return default

if "darwin" == os.sys.platform:
    darwin = True
    platform = "osx" # prettier than darwin

    env.Append( CPPPATH=[ "-I/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Headers/" ] )

    env.Append( CPPFLAGS=" -mmacosx-version-min=10.4 " )
    if not nojni:
        env.Append( FRAMEWORKS=["JavaVM"] )

    if os.path.exists( "/usr/bin/g++-4.2" ):
        env["CXX"] = "g++-4.2"

    nix = True

    if force64:
        env.Append( CPPPATH=["/usr/64/include"] )
        env.Append( LIBPATH=["/usr/64/lib"] )
        if installDir == DEFAULT_INSTALl_DIR and not distBuild:
            installDir = "/usr/64/"
    else:
        env.Append( CPPPATH=[ "/sw/include" , "/opt/local/include"] )
        env.Append( LIBPATH=["/sw/lib/", "/opt/local/lib"] )

elif "linux2" == os.sys.platform:
    linux = True
    useJavaHome = True
    javaOS = "linux"
    platform = "linux"

    javaHome = choosePathExist( [ javaHome , "/usr/lib/jvm/java/" , os.environ.get( "JAVA_HOME" ) ] , "/usr/lib/jvm/java/" )

    if os.uname()[4] == "x86_64" and not force32:
        linux64 = True
        javaVersion = "amd64"
        nixLibPrefix = "lib64"
        env.Append( LIBPATH=["/usr/lib64" , "/lib64" ] )
        env.Append( LIBS=["pthread"] )

        force64 = False

    if force32:
        env.Append( LIBPATH=["/usr/lib32"] )

    nix = True

    if static:
        env.Append( LINKFLAGS=" -static " )

elif "sunos5" == os.sys.platform:
     nix = True
     solaris = True
     useJavaHome = True
     javaHome = "/usr/lib/jvm/java-6-sun/"
     javaOS = "solaris"
     env.Append( CPPDEFINES=[ "__linux__" , "__sunos__" ] )
     env.Append( LIBS=["socket","resolv"] )

elif "freebsd7" == os.sys.platform:
    nix = True
    freebsd = True
    env.Append( CPPPATH=[ "/usr/local/include" ] )
    env.Append( LIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__freebsd__" ] )

elif "win32" == os.sys.platform:
    windows = True
    if force64:
        release = True

    for bv in reversed( range(3,10) ):
        boostDir = "C:/Program Files/Boost/boost_1_3" + str(bv) + "_0"
        if os.path.exists( boostDir ):
            break

    serverOnlyFiles += [ "util/ntservice.cpp" ]

    if not os.path.exists( boostDir ):
        print( "can't find boost" )
        Exit(1)

    boostLibs = []

    if usesm:
        env.Append( CPPPATH=[ "js/src/" ] )
        env.Append(CPPPATH=["../js/src/"])
        env.Append(LIBPATH=["../js/src"])
        env.Append( CPPDEFINES=[ "OLDJS" ] )
    else:
        javaHome = findVersion( "C:/Program Files/java/" ,
                                [ "jdk" , "jdk1.6.0_10" ] )
        env.Append( CPPPATH=[ javaHome + "/include" , javaHome + "/include/win32" ] )
        env.Append( LIBPATH=[ javaHome + "/Lib" ] )
        javaLibs += [ "jvm" ];

    winSDKHome = findVersion( "C:/Program Files/Microsoft SDKs/Windows/" ,
                              [ "v6.0" , "v6.0a" , "v6.1" ] )

    env.Append( CPPPATH=[ boostDir , "pcre-7.4" , winSDKHome + "/Include" ] )

    env.Append( CPPFLAGS=" /EHsc /W3 " )
    env.Append( CPPFLAGS=" /wd4355 /wd4800 " ) #some warnings we don't like
    env.Append( CPPDEFINES=["WIN32","_CONSOLE","_CRT_SECURE_NO_WARNINGS","HAVE_CONFIG_H","PCRE_STATIC","_UNICODE","UNICODE","SUPPORT_UCP","SUPPORT_UTF8" ] )

    #env.Append( CPPFLAGS='  /Yu"stdafx.h" ' ) # this would be for pre-compiled headers, could play with it later

    if release:
        env.Append( CPPDEFINES=[ "NDEBUG" ] )
        env.Append( CPPFLAGS= " /O2 /Oi /FD /MT /Gy /nologo /Zi /TP /errorReport:prompt /Gm " )
        #env.Append( CPPFLAGS= " /GL " ) # TODO: this has caused some linking problems
    else:
        env.Append( CPPDEFINES=[ "_DEBUG" ] )
        env.Append( CPPFLAGS=" /Od /Gm /RTC1 /MDd /ZI " )
        env.Append( CPPFLAGS=' /Fd"mongod.pdb" ' )

    env.Append( LIBPATH=[ boostDir + "/Lib" ] )
    if force64:
        env.Append( LIBPATH=[ winSDKHome + "/Lib/x64" ] )
        env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  /NODEFAULTLIB:MSVCRT " )
    else:
        env.Append( LIBPATH=[ winSDKHome + "/Lib" ] )


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

    pcreFiles = []
    pcreFiles += filter( pcreFilter , Glob( "pcre-7.4/*.c"  ) )
    pcreFiles += filter( pcreFilter , Glob( "pcre-7.4/*.cc" ) )
    commonFiles += pcreFiles
    allClientFiles += pcreFiles

    winLibString = "ws2_32.lib kernel32.lib advapi32.lib"
    if force64:
        winLibString += " LIBCMT LIBCPMT "
    else:
        winLibString += " user32.lib gdi32.lib winspool.lib comdlg32.lib  shell32.lib ole32.lib oleaut32.lib "
        winLibString += " odbc32.lib odbccp32.lib uuid.lib "
    env.Append( LIBS=Split(winLibString) )

    if force64:
        env.Append( CPPDEFINES=["_AMD64_=1"] )
    else:
        env.Append( CPPDEFINES=["_X86_=1"] )

    env.Append( CPPPATH=["../winpcap/Include"] )
    env.Append( LIBPATH=["../winpcap/Lib"] )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )

if not nojni and useJavaHome:
    env.Append( CPPPATH=[ javaHome + "include" , javaHome + "include/" + javaOS ] )
    env.Append( LIBPATH=[ javaHome + "jre/lib/" + javaVersion + "/server" , javaHome + "jre/lib/" + javaVersion ] )

    if not nojni:
        javaLibs += [ "java" , "jvm" ]

    env.Append( LINKFLAGS="-Xlinker -rpath -Xlinker " + javaHome + "jre/lib/" + javaVersion + "/server" )
    env.Append( LINKFLAGS="-Xlinker -rpath -Xlinker " + javaHome + "jre/lib/" + javaVersion  )

if nix:
    env.Append( CPPFLAGS="-fPIC -fno-strict-aliasing -ggdb -pthread -Wall -Wsign-compare -Wno-unknown-pragmas -Winvalid-pch" )
    env.Append( CXXFLAGS=" -Wnon-virtual-dtor " )
    env.Append( LINKFLAGS=" -fPIC -pthread " )
    env.Append( LIBS=[] )

    if debugBuild:
        env.Append( CPPFLAGS=" -O0 -fstack-protector -fstack-check" );
    else:
        env.Append( CPPFLAGS=" -O3" )

    if debugLogging:
        env.Append( CPPFLAGS=" -D_DEBUG" );

    if force64:
        env.Append( CFLAGS="-m64" )
        env.Append( CXXFLAGS="-m64" )
        env.Append( LINKFLAGS="-m64" )

    if force32:
        env.Append( CFLAGS="-m32" )
        env.Append( CXXFLAGS="-m32" )
        env.Append( LINKFLAGS="-m32" )

    if GetOption( "profile" ) is not None:
        env.Append( LINKFLAGS=" -pg " )

    # pre-compiled headers
    if False and 'Gch' in dir( env ):
        print( "using precompiled headers" )
        env['Gch'] = env.Gch( [ "stdafx.h" ] )[0]
        #Depends( "stdafx.o" , "stdafx.h.gch" )
        #SideEffect( "dummyGCHSideEffect" , "stdafx.h.gch" )
        

if "uname" in dir(os):
    hacks = buildscripts.findHacks( os.uname() )
    if hacks is not None:
        hacks.insert( env , { "linux64" : linux64 } )
        
try:
    umask = os.umask(022)
except OSError:
    pass

# --- check system ---

def getGitVersion():
    if not os.path.exists( ".git" ):
        return "nogitversion"

    version = open( ".git/HEAD" ,'r' ).read().strip()
    if not version.startswith( "ref: " ):
        return version
    version = version[5:]
    f = ".git/" + version
    if not os.path.exists( f ):
        return version
    return open( f , 'r' ).read().strip()

def getSysInfo():
    import os, sys
    if windows:
        return "windows " + str( sys.getwindowsversion() )
    else:
        return " ".join( os.uname() )

def setupBuildInfoFile( outFile ):
    version = getGitVersion()
    sysInfo = getSysInfo()
    contents = "#include \"stdafx.h\"\n"
    contents += "#include <iostream>\n"
    contents += "namespace mongo { const char * gitVersion(){ return \"" + version + "\"; } }\n"
    contents += "namespace mongo { const char * sysInfo(){ return \"" + sysInfo + "\"; } }\n"

    if os.path.exists( outFile ) and open( outFile ).read().strip() == contents.strip():
        return

    out = open( outFile , 'w' )
    out.write( contents )
    out.close()

setupBuildInfoFile( "buildinfo.cpp" )

def bigLibString( myenv ):
    s = str( myenv["LIBS"] )
    if 'SLIBS' in myenv._dict:
        s += str( myenv["SLIBS"] )
    return s


def doConfigure( myenv , needJava=True , needPcre=True , shell=False ):
    conf = Configure(myenv)
    myenv["LINKFLAGS_CLEAN"] = list( myenv["LINKFLAGS"] )
    myenv["LIBS_CLEAN"] = list( myenv["LIBS"] )

    if 'CheckCXX' in dir( conf ):
        if  not conf.CheckCXX():
            print( "c++ compiler not installed!" )
            Exit(1)

    if nix and not shell:
        if not conf.CheckLib( "stdc++" ):
            print( "can't find stdc++ library which is needed" );
            Exit(1)

    def myCheckLib( poss , failIfNotFound=False , java=False , staticOnly=False):

        if type( poss ) != types.ListType :
            poss = [poss]

        allPlaces = [];
        if nix and release:
            allPlaces += myenv["LIBPATH"]
            if not force64:
                allPlaces += [ "/usr/lib" , "/usr/local/lib" ]

            for p in poss:
                for loc in allPlaces:
                    fullPath = loc + "/lib" + p + ".a"
                    if os.path.exists( fullPath ):
                        myenv['_LIBFLAGS']='${_stripixes(LIBLINKPREFIX, LIBS, LIBLINKSUFFIX, LIBPREFIXES, LIBSUFFIXES, __env__)} $SLIBS'
                        myenv.Append( SLIBS=" " + fullPath + " " )
                        return True


        if release and not java and not windows and failIfNotFound:
            print( "ERROR: can't find static version of: " + str( poss ) + " in: " + str( allPlaces ) )
            Exit(1)

        res = not staticOnly and conf.CheckLib( poss )
        if res:
            return True

        if failIfNotFound:
            print( "can't find " + str( poss ) + " in " + str( myenv["LIBPATH"] ) )
            Exit(1)

        return False

    if needPcre and not conf.CheckCXXHeader( 'pcrecpp.h' ):
        print( "can't find pcre" )
        Exit(1)

    if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
        print( "can't find boost headers" )
        if shell:
            print( "\tshell might not compile" )
        else:
            Exit(1)

    if conf.CheckCXXHeader( "boost/asio.hpp" ):
        # TODO: turn this back on when ASIO working
        myenv.Append( CPPDEFINES=[ "USE_ASIO_OFF" ] )
    else:
        print( "WARNING: old version of boost - you should consider upgrading" )

    for b in boostLibs:
        l = "boost_" + b
        myCheckLib( [ l + boostCompiler + "-mt" + boostVersion , 
                      l + boostCompiler + boostVersion ] , 
                    release or not shell)

    # this will add it iff it exists and works
    myCheckLib( "boost_system" + boostCompiler + "-mt" + boostVersion )

    if not conf.CheckCXXHeader( "execinfo.h" ):
        myenv.Append( CPPDEFINES=[ "NOEXECINFO" ] )

    if needJava:
        for j in javaLibs:
            myCheckLib( j , True , True )

    if nix and needPcre:
        myCheckLib( "pcrecpp" , True )
        myCheckLib( "pcre" , True )

    myenv["_HAVEPCAP"] = myCheckLib( ["pcap", "wpcap"] )
    removeIfInList( myenv["LIBS"] , "pcap" )
    removeIfInList( myenv["LIBS"] , "wpcap" )

    # this is outside of usesm block so don't have to rebuild for java
    if windows:
        myenv.Append( CPPDEFINES=[ "XP_WIN" ] )
    else:
        myenv.Append( CPPDEFINES=[ "XP_UNIX" ] )

    if solaris:
        conf.CheckLib( "nsl" )

    if usesm:

        myCheckLib( [ "js" , "mozjs" ] , True )
        mozHeader = "js"
        if bigLibString(myenv).find( "mozjs" ) >= 0:
            mozHeader = "mozjs"

        if not conf.CheckHeader( mozHeader + "/jsapi.h" ):
            if conf.CheckHeader( "jsapi.h" ):
                myenv.Append( CPPDEFINES=[ "OLDJS" ] )
            else:
                print( "no spider monkey headers!" )
                Exit(1)

    if shell:
        haveReadLine = False
        if darwin:
            myenv.Append( CPPDEFINES=[ "USE_READLINE" ] )
            if force64:
                myCheckLib( "readline" , True )
                myCheckLib( "ncurses" , True )
            else:
                myenv.Append( LINKFLAGS=" /usr/lib/libreadline.dylib " )
        elif myCheckLib( "readline" , release and nix , staticOnly=release ):
            myenv.Append( CPPDEFINES=[ "USE_READLINE" ] )
            myCheckLib( "ncurses" , staticOnly=release )
            myCheckLib( "tinfo" , staticOnly=release )
        else:
            print( "warning: no readline, shell will be a bit ugly" )

        if linux:
            myCheckLib( "rt" , True )

    # requires ports devel/libexecinfo to be installed
    if freebsd:
        myCheckLib( "execinfo", True )
        env.Append( LIBS=[ "execinfo" ] )

    return conf.Finish()

env = doConfigure( env )

# --- js concat ---

def concatjs(target, source, env):

    outFile = str( target[0] )

    fullSource = ""

    for s in source:
        f = open( str(s) , 'r' )
        for l in f:
            fullSource += l

    out = open( outFile , 'w' )
    out.write( fullSource )

    return None

jsBuilder = Builder(action = concatjs,
                    suffix = '.jsall',
                    src_suffix = '.js')

env.Append( BUILDERS={'JSConcat' : jsBuilder})

# --- jsh ---

def jsToH(target, source, env):

    outFile = str( target[0] )
    if len( source ) != 1:
        raise Exception( "wrong" )

    h = "const char * jsconcatcode = \n"

    for l in open( str(source[0]) , 'r' ):
        l = l.strip()
        l = l.partition( "//" )[0]
        l = l.replace( '\\' , "\\\\" )
        l = l.replace( '"' , "\\\"" )


        h += '"' + l + "\\n\"\n "

    h += ";\n\n"

    out = open( outFile , 'w' )
    out.write( h )

    return None

jshBuilder = Builder(action = jsToH,
                    suffix = '.cpp',
                    src_suffix = '.jsall')

env.Append( BUILDERS={'JSHeader' : jshBuilder})


# --- targets ----

clientEnv = env.Clone();
clientEnv.Append( CPPPATH=["../"] )
clientEnv.Prepend( LIBS=[ "mongoclient"] )
clientEnv.Prepend( LIBPATH=["."] )
l = clientEnv[ "LIBS" ]
removeIfInList( l , "pcre" )
removeIfInList( l , "pcrecpp" )

testEnv = env.Clone()
testEnv.Append( CPPPATH=["../"] )
testEnv.Prepend( LIBS=[ "mongotestfiles" ] )
testEnv.Prepend( LIBPATH=["."] )


# ----- TARGETS ------


# main db target
mongod = env.Program( "mongod" , commonFiles + coreDbFiles + serverOnlyFiles + [ "db/db.cpp" , "db/mms.cpp" ]  )
Default( mongod )

# tools
allToolFiles = commonFiles + coreDbFiles + serverOnlyFiles + [ "client/gridfs.cpp", "tools/Tool.cpp" ]
env.Program( "mongodump" , allToolFiles + [ "tools/dump.cpp" ] )
env.Program( "mongorestore" , allToolFiles + [ "tools/restore.cpp" ] )

env.Program( "mongoexport" , allToolFiles + [ "tools/export.cpp" ] )
env.Program( "mongoimportjson" , allToolFiles + [ "tools/importJSON.cpp" ] )

env.Program( "mongofiles" , allToolFiles + [ "tools/files.cpp" ] )

env.Program( "mongobridge" , allToolFiles + [ "tools/bridge.cpp" ] )

# mongos
mongos = env.Program( "mongos" , commonFiles + coreDbFiles + coreServerFiles + shardServerFiles )

# c++ library
clientLibName = str( env.Library( "mongoclient" , allClientFiles )[0] )
env.Library( "mongotestfiles" , commonFiles + coreDbFiles + serverOnlyFiles )

clientTests = []

# examples
clientTests += [ clientEnv.Program( "firstExample" , [ "client/examples/first.cpp" ] ) ]
clientTests += [ clientEnv.Program( "secondExample" , [ "client/examples/second.cpp" ] ) ]
clientTests += [ clientEnv.Program( "whereExample" , [ "client/examples/whereExample.cpp" ] ) ]
clientTests += [ clientEnv.Program( "authTest" , [ "client/examples/authTest.cpp" ] ) ]

# testing
test = testEnv.Program( "test" , Glob( "dbtests/*.cpp" ) )
perftest = testEnv.Program( "perftest", [ "dbtests/framework.cpp" , "dbtests/perf/perftest.cpp" ] )
clientTests += [ clientEnv.Program( "clientTest" , [ "client/examples/clientTest.cpp" ] ) ]

# --- sniffer ---
mongosniff_built = False
if darwin or clientEnv["_HAVEPCAP"]:
    mongosniff_built = True
    sniffEnv = clientEnv.Clone()
    if not windows:
        sniffEnv.Append( LIBS=[ "pcap" ] )
    else:
        sniffEnv.Append( LIBS=[ "wpcap" ] )
    sniffEnv.Program( "mongosniff" , "tools/sniffer.cpp" )

# --- shell ---

env.JSConcat( "shell/mongo.jsall"  , Glob( "shell/*.js" ) )
env.JSHeader( "shell/mongo.jsall" )

shellEnv = env.Clone();

if release and ( ( darwin and force64 ) or linux64 ):
    shellEnv["LINKFLAGS"] = env["LINKFLAGS_CLEAN"]
    shellEnv["LIBS"] = env["LIBS_CLEAN"]
    shellEnv["SLIBS"] = ""

if noshell:
    print( "not building shell" )
elif not onlyServer:
    weird = force64 and not windows and not solaris

    if weird:
        shellEnv["CFLAGS"].remove("-m64")
        shellEnv["CXXFLAGS"].remove("-m64")
        shellEnv["LINKFLAGS"].remove("-m64")
        shellEnv["CPPPATH"].remove( "/usr/64/include" )
        shellEnv["LIBPATH"].remove( "/usr/64/lib" )
        shellEnv.Append( CPPPATH=[ "/sw/include" , "/opt/local/include"] )
        shellEnv.Append( LIBPATH=[ "/sw/lib/", "/opt/local/lib" , "/usr/lib" ] )

    l = shellEnv["LIBS"]
    if linux64:
        removeIfInList( l , "java" )
        removeIfInList( l , "jvm" )

    removeIfInList( l , "pcre" )
    removeIfInList( l , "pcrecpp" )

    if windows:
        shellEnv.Append( LIBS=["winmm.lib"] )

    coreShellFiles = [ "shell/dbshell.cpp" , "shell/utils.cpp" ]

    if weird:
        shell32BitFiles = coreShellFiles
        for f in allClientFiles:
            shell32BitFiles.append( "32bit/" + str( f ) )

        shellEnv.VariantDir( "32bit" , "." )
    else:
        shellEnv.Prepend( LIBPATH=[ "." ] )

    shellEnv = doConfigure( shellEnv , needPcre=False , needJava=False , shell=True )

    if weird:
        mongo = shellEnv.Program( "mongo" , shell32BitFiles )
    else:
        shellEnv.Prepend( LIBS=[ "mongoclient"] )
        mongo = shellEnv.Program( "mongo" , coreShellFiles )

    if weird:
        Depends( "32bit/shell/mongo.cpp" , "shell/mongo.cpp" )


#  ---- RUNNING TESTS ----

testEnv.Alias( "dummySmokeSideEffect", [], [] )

def addSmoketest( name, deps, actions ):
    if type( actions ) == type( list() ):
        actions = [ testSetup ] + actions
    else:
        actions = [ testSetup, actions ]
    testEnv.Alias( name, deps, actions )
    testEnv.AlwaysBuild( name )
    # Prevent smoke tests from running in parallel
    testEnv.SideEffect( "dummySmokeSideEffect", name )

def ensureDir( name ):
    d = os.path.dirname( name )
    if not os.path.exists( d ):
        print( "Creating dir: " + name );
        os.makedirs( d )
        if not os.path.exists( d ):
            print( "Failed to create dir: " + name );
            Exit( 1 )

def ensureTestDirs():
    ensureDir( "/tmp/unittest/" )
    ensureDir( "/data/" )
    ensureDir( "/data/db/" )

def testSetup( env , target , source ):
    ensureTestDirs()

if len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) == "test":
    ensureDir( "/tmp/unittest/" );

addSmoketest( "smoke", [ "test" ] , [ test[ 0 ].abspath ] )
addSmoketest( "smokePerf", [ "perftest" ] , [ perftest[ 0 ].abspath ] )

clientExec = [ x[0].abspath for x in clientTests ]
def runClientTests( env, target, source ):
    global clientExec
    global mongodForTestsPort
    import subprocess
    for i in clientExec:
        if subprocess.call( [ i, "--port", mongodForTestsPort ] ) != 0:
            return True
    if subprocess.Popen( [ mongod[0].abspath, "msg", "ping", mongodForTestsPort ], stdout=subprocess.PIPE ).communicate()[ 0 ].count( "****ok" ) == 0:
        return True
    if subprocess.call( [ mongod[0].abspath, "msg", "ping", mongodForTestsPort ] ) != 0:
        return True
    return False
addSmoketest( "smokeClient" , clientExec, runClientTests )
addSmoketest( "mongosTest" , [ mongos[0].abspath ] , [ mongos[0].abspath + " --test" ] )

def jsSpec( suffix ):
    import os.path
    args = [ os.path.dirname( mongo[0].abspath ), "jstests" ] + suffix
    return apply( os.path.join, args )

def jsDirTestSpec( dir ):
    return mongo[0].abspath + " --nodb " + jsSpec( [ dir, "*.js" ] )

def runShellTest( env, target, source ):
    global mongodForTestsPort
    import subprocess
    target = str( target[0] )
    if target == "smokeJs":
        spec = [ jsSpec( [ "_runner.js" ] ) ]
    elif target == "smokeQuota":
        g = Glob( jsSpec( [ "quota", "*.js" ] ) )
        spec = [ x.abspath for x in g ]
    elif target == "smokeJsPerf":
        g = Glob( jsSpec( [ "perf", "*.js" ] ) )
        spec = [ x.abspath for x in g ]
    else:
        print( "invalid target for runShellTest()" )
        Exit( 1 )
    return subprocess.call( [ mongo[0].abspath, "--port", mongodForTestsPort ] + spec )

def add_exe(target):
    if windows:
        return target + ".exe"
    return target

# These tests require the mongo shell
if not onlyServer and not noshell:
    addSmoketest( "smokeJs", [add_exe("mongo")], runShellTest )
    addSmoketest( "smokeClone", [ "mongo", "mongod" ], [ jsDirTestSpec( "clone" ) ] )
    addSmoketest( "smokeRepl", [ "mongo", "mongod", "mongobridge" ], [ jsDirTestSpec( "repl" ) ] )
    addSmoketest( "smokeDisk", [ "mongo", "mongod" ], [ jsDirTestSpec( "disk" ) ] )
    addSmoketest( "smokeSharding", [ "mongo", "mongod", "mongos" ], [ jsDirTestSpec( "sharding" ) ] )
    addSmoketest( "smokeJsPerf", [ "mongo" ], runShellTest )
    addSmoketest( "smokeQuota", [ "mongo" ], runShellTest )
    addSmoketest( "smokeTool", [ "mongo" ], [ jsDirTestSpec( "tool" ) ] )

mongodForTests = None
mongodForTestsPort = "27017"

def startMongodForTests( env, target, source ):
    global mongodForTests
    global mongodForTestsPort
    global mongod
    if mongodForTests:
        return
    mongodForTestsPort = "40000"
    import os
    ensureTestDirs()
    dirName = "/data/db/sconsTests/"
    ensureDir( dirName )
    from subprocess import Popen
    mongodForTests = Popen( [ mongod[0].abspath, "--port", mongodForTestsPort, "--dbpath", dirName ] )
    # Wait for mongod to start
    import time
    time.sleep( 5 )
    if mongodForTests.poll() is not None:
        print( "Failed to start mongod" )
        mongodForTests = None
        Exit( 1 )

def stopMongodForTests():
    global mongodForTests
    if not mongodForTests:
        return
    if mongodForTests.poll() is not None:
        print( "Failed to start mongod" )
        mongodForTests = None
        Exit( 1 )
    try:
        # This function not available in Python 2.5
        mongodForTests.terminate()
    except AttributeError:
        if windows:
            import win32process
            win32process.TerminateProcess(mongodForTests._handle, -1)
        else:
            from os import kill
            kill( mongodForTests.pid, 15 )
    mongodForTests.wait()

testEnv.Alias( "startMongod", [add_exe("mongod")], [startMongodForTests] );
testEnv.AlwaysBuild( "startMongod" );
testEnv.SideEffect( "dummySmokeSideEffect", "startMongod" )

def addMongodReqTargets( env, target, source ):
    mongodReqTargets = [ "smokeClient", "smokeJs", "smokeQuota" ]
    for target in mongodReqTargets:
        testEnv.Depends( target, "startMongod" )
        testEnv.Depends( "smokeAll", target )

testEnv.Alias( "addMongodReqTargets", [], [addMongodReqTargets] )
testEnv.AlwaysBuild( "addMongodReqTargets" )

testEnv.Alias( "smokeAll", [ "smoke", "mongosTest", "smokeClone", "smokeRepl", "addMongodReqTargets", "smokeDisk", "smokeSharding", "smokeTool" ] )
testEnv.AlwaysBuild( "smokeAll" )

def addMongodReqNoJsTargets( env, target, source ):
    mongodReqTargets = [ "smokeClient" ]
    for target in mongodReqTargets:
        testEnv.Depends( target, "startMongod" )
        testEnv.Depends( "smokeAllNoJs", target )

testEnv.Alias( "addMongodReqNoJsTargets", [], [addMongodReqNoJsTargets] )
testEnv.AlwaysBuild( "addMongodReqNoJsTargets" )

testEnv.Alias( "smokeAllNoJs", [ "smoke", "mongosTest", "addMongodReqNoJsTargets" ] )
testEnv.AlwaysBuild( "smokeAllNoJs" )

import atexit
atexit.register( stopMongodForTests )

def machine_info(extra_info=""):
    """Get a dict representing the "machine" section of a benchmark result.

    ie:
    {
        "os_name": "OS X",
        "os_version": "10.5",
        "processor": "2.4 GHz Intel Core 2 Duo",
        "memory": "3 GB 667 MHz DDR2 SDRAM",
        "extra_info": "Python 2.6"
    }

    Must have a settings.py file on sys.path that defines "processor" and "memory"
    variables.
    """
    sys.path.append("")
    import settings

    machine = {}
    (machine["os_name"], _, machine["os_version"], _, _) = os.uname()
    machine["processor"] = settings.processor
    machine["memory"] = settings.memory
    machine["extra_info"] = extra_info
    return machine

def post_data(data, machine_extra_info="", post_url="http://mongo-db.appspot.com/benchmark"):
    """Post a benchmark data point.

    data should be a Python dict that looks like:
        {
          "benchmark": {
            "project": "http://github.com/mongodb/mongo-python-driver",
            "name": "insert test",
            "description": "test inserting 10000 documents with the C extension enabled",
            "tags": ["insert", "python"]
          },
          "trial": {
            "server_hash": "4f5a8d52f47507a70b6c625dfb5dbfc87ba5656a",
            "client_hash": "8bf2ad3d397cbde745fd92ad41c5b13976fac2b5",
            "result": 67.5,
            "extra_info": "some logs or something"
          }
        }
    """
    try:
        import json
    except:
        import simplejson as json # needed for python < 2.6

    data["machine"] = machine_info(machine_extra_info)
    print( data )
    urllib2.urlopen(post_url, urllib.urlencode({"payload": json.dumps(data)}))

def recordPerformance( env, target, source ):
    global perftest
    import subprocess, re
    p = subprocess.Popen( [ perftest[0].abspath ], stdout=subprocess.PIPE )
    b = p.communicate()[ 0 ]
    print( "perftest results:" );
    print( b );
    if p.returncode != 0:
        return True
    entries = re.findall( "{.*?}", b )
    import sys
    for e in entries:
        matches = re.match( "{'(.*?)': (.*?)}", e )
        name = matches.group( 1 )
        val = float( matches.group( 2 ) )
        sub = { "benchmark": { "project": "http://github.com/mongodb/mongo", "description": "" }, "trial": {} }
        sub[ "benchmark" ][ "name" ] = name
        sub[ "benchmark" ][ "tags" ] = [ "c++", re.match( "(.*)__", name ).group( 1 ) ]
        sub[ "trial" ][ "server_hash" ] = getGitVersion()
        sub[ "trial" ][ "client_hash" ] = ""
        sub[ "trial" ][ "result" ] = val
        try:
            post_data(sub)
        except:
            print( "exception posting perf results" )
            print( sys.exc_info() )
    return False

addSmoketest( "recordPerf", [ "perftest" ] , [ recordPerformance ] )

from buildscripts import test_shell
def run_shell_tests(env, target, source):
    test_shell.mongo_path = windows and "mongo.exe" or "mongo"
    test_shell.run_tests()

env.Alias("test_shell", [], [run_shell_tests])
env.AlwaysBuild("test_shell")

#  ----  INSTALL -------

def getSystemInstallName():
    n = platform + "-" + processor
    if static:
        n += "-static"
    if nix and os.uname()[2].startswith( "8." ):
        n += "-tiger"
    return n


def getCodeVersion():
    fullSource = open( "stdafx.cpp" , "r" ).read()
    allMatches = re.findall( r"versionString.. = \"(.*?)\"" , fullSource );
    if len(allMatches) != 1:
        print( "can't find version # in code" )
        return None
    return allMatches[0]

def getDistName( sofar ):
    global distName
    global dontReplacePackage

    if distName is not None:
        return distName

    if str( COMMAND_LINE_TARGETS[0] ) == "s3dist":
        version = getCodeVersion()
        if not version.endswith( "+" ) and not version.endswith("-"):
            print( "got real code version, doing release build for: " + version )
            dontReplacePackage = True
            distName = version
            return version

    return today.strftime( "%Y-%m-%d" )

if distBuild:
    from datetime import date
    today = date.today()
    installDir = "mongodb-" + getSystemInstallName() + "-"
    installDir += getDistName( installDir )
    print "going to make dist: " + installDir

# binaries

def checkGlibc(target,source,env):
    import subprocess
    stringProcess = subprocess.Popen( [ "strings" , str( target[0] ) ] , stdout=subprocess.PIPE )
    stringResult = stringProcess.communicate()[0]
    if stringResult.count( "GLIBC_2.4" ) > 0:
        print( str( target[0] ) + " has GLIBC_2.4 dependencies!" )
        Exit(-3)

allBinaries = []

def installBinary( e , name ):
    global allBinaries

    if windows:
        name += ".exe"

    inst = e.Install( installDir + "/bin" , name )

    fullInstallName = installDir + "/bin/" + name

    allBinaries += [ name ]
    if solaris or linux:
        e.AddPostAction( inst, e.Action( 'strip ' + fullInstallName ) )

    if linux and len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) == "s3dist":
        e.AddPostAction( inst , checkGlibc )

    if nix:
        e.AddPostAction( inst , e.Action( 'chmod 755 ' + fullInstallName ) )

installBinary( env , "mongodump" )
installBinary( env , "mongorestore" )

installBinary( env , "mongoexport" )
installBinary( env , "mongoimportjson" )

installBinary( env , "mongofiles" )

if mongosniff_built:
    installBinary(env, "mongosniff")

installBinary( env , "mongod" )
installBinary( env , "mongos" )

if not noshell:
    installBinary( env , "mongo" )

env.Alias( "all" , allBinaries )


# NOTE: In some cases scons gets confused between installation targets and build
# dependencies.  Here, we use InstallAs instead of Install to prevent such confusion
# on a case-by-case basis.

#headers
for id in [ "", "util/", "db/" , "client/" ]:
    env.Install( installDir + "/include/mongo/" + id , Glob( id + "*.h" ) )

#lib
env.Install( installDir + "/" + nixLibPrefix, clientLibName )
if usejvm:
    env.Install( installDir + "/" + nixLibPrefix + "/mongo/jars" , Glob( "jars/*" ) )

#textfiles
if distBuild or release:
    #don't want to install these /usr/local/ for example
    env.Install( installDir , "distsrc/README" )
    env.Install( installDir , "distsrc/THIRD-PARTY-NOTICES" )
    env.Install( installDir , "distsrc/GNU-AGPL-3.0" )

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


# ---- deploying ---

def s3push( localName , remoteName=None , remotePrefix=None , fixName=True , platformDir=True ):

    if remotePrefix is None:
        if distName is None:
            remotePrefix = "-latest"
        else:
            remotePrefix = "-" + distName

    sys.path.append( "." )
    sys.path.append( ".." )
    sys.path.append( "../../" )

    import simples3
    import settings

    s = simples3.S3Bucket( settings.bucket , settings.id , settings.key )

    if remoteName is None:
        remoteName = localName

    if fixName:
        (root,dot,suffix) = localName.rpartition( "." )
        name = remoteName + "-" + getSystemInstallName()
        name += remotePrefix
        if dot == "." :
            name += "." + suffix
        name = name.lower()
    else:
        name = remoteName

    if platformDir:
        name = platform + "/" + name

    print( "uploading " + localName + " to http://s3.amazonaws.com/" + s.name + "/" + name )
    if dontReplacePackage:
        for ( key , modify , etag , size ) in s.listdir( prefix=name ):
            print( "error: already a file with that name, not uploading" )
            Exit(2)
    s.put( name  , open( localName , "rb" ).read() , acl="public-read" );
    print( "  done uploading!" )

def s3shellpush( env , target , source ):
    s3push( "mongo" , "mongo-shell" )

env.Alias( "s3shell" , [ "mongo" ] , [ s3shellpush ] )
env.AlwaysBuild( "s3shell" )

def s3dist( env , target , source ):
    s3push( distFile , "mongodb" )

env.Append( TARFLAGS=" -z " )
if windows:
    distFile = installDir + ".zip"
    env.Zip( distFile , installDir )
else:
    distFile = installDir + ".tgz"
    env.Tar( distFile , installDir )

env.Alias( "dist" , distFile )
env.Alias( "s3dist" , [ "install"  , distFile ] , [ s3dist ] )
env.AlwaysBuild( "s3dist" )

def clean_old_dist_builds(env, target, source):
    prefix = "mongodb-%s-%s" % (platform, processor)
    filenames = sorted(os.listdir("."))
    filenames = [x for x in filenames if x.startswith(prefix)]
    to_keep = [x for x in filenames if x.endswith(".tgz") or x.endswith(".zip")][-2:]
    for filename in [x for x in filenames if x not in to_keep]:
        print "removing %s" % filename
        try:
            shutil.rmtree(filename)
        except:
            os.remove(filename)

env.Alias("dist_clean", [], [clean_old_dist_builds])
env.AlwaysBuild("dist_clean")
