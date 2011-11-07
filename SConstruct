# -*- mode: python; -*-
# build file for MongoDB
# this requires scons
# you can get from http://www.scons.org
# then just type scons

# some common tasks
#   build 64-bit mac and pushing to s3
#      scons --64 s3dist
#      scons --distname=0.8 s3dist
#      all s3 pushes require settings.py and simples3

EnsureSConsVersion(0, 98, 4) # this is a common version known to work

import os
import sys
import imp
import types
import re
import shutil
import urllib
import urllib2
import buildscripts
import buildscripts.bb
import stat
from buildscripts import utils

buildscripts.bb.checkOk()

def findSettingsSetup():
    sys.path.append( "." )
    sys.path.append( ".." )
    sys.path.append( "../../" )

# --- options ----

options = {}

options_topass = {}

def add_option( name, help , nargs , contibutesToVariantDir , dest=None ):

    if dest is None:
        dest = name

    AddOption( "--" + name , 
               dest=dest,
               type="string",
               nargs=nargs,
               action="store",
               help=help )

    options[name] = { "help" : help ,
                      "nargs" : nargs , 
                      "contibutesToVariantDir" : contibutesToVariantDir ,
                      "dest" : dest } 

def get_option( name ):
    return GetOption( name )

def _has_option( name ):
    x = get_option( name )
    if x is None:
        return False

    if x == False:
        return False

    if x == "":
        return False

    return True

def has_option( name ):
    x = _has_option(name)
    options_topass[name] = x
    return x


def get_variant_dir():
    
    a = []
    
    for name in options:
        o = options[name]
        if not has_option( o["dest"] ):
            continue
        if not o["contibutesToVariantDir"]:
            continue
        
        if o["nargs"] == 0:
            a.append( name )
        else:
            a.append( name + "-" + get_option( name ) )

    s = "build/"

    if len(a) > 0:
        a.sort()
        s += "/".join( a ) + "/"
        
    return s
        


# installation/packaging
add_option( "prefix" , "installation prefix" , 1 , False )
add_option( "distname" , "dist name (0.8.0)" , 1 , False )
add_option( "distmod", "additional piece for full dist name" , 1 , False )
add_option( "nostrip", "do not strip installed binaries" , 0 , False )

add_option( "sharedclient", "build a libmongoclient.so/.dll" , 0 , False )
add_option( "full", "include client and headers when doing scons install", 0 , False )

# linking options
add_option( "release" , "release build" , 0 , True )
add_option( "static" , "fully static build" , 0 , True )

# base compile flags
add_option( "64" , "whether to force 64 bit" , 0 , True , "force64" )
add_option( "32" , "whether to force 32 bit" , 0 , True , "force32" )

add_option( "cxx", "compiler to use" , 1 , True )
add_option( "cc", "compiler to use for c" , 1 , True )

add_option( "cpppath", "Include path if you have headers in a nonstandard directory" , 1 , True )
add_option( "libpath", "Library path if you have libraries in a nonstandard directory" , 1 , True )

add_option( "extrapath", "comma separated list of add'l paths  (--extrapath /opt/foo/,/foo) static linking" , 1 , True )
add_option( "extrapathdyn", "comma separated list of add'l paths  (--extrapath /opt/foo/,/foo) dynamic linking" , 1 , True )
add_option( "extralib", "comma separated list of libraries  (--extralib js_static,readline" , 1 , True )
add_option( "staticlib", "comma separated list of libs to link statically (--staticlib js_static,boost_program_options-mt,..." , 1 , True )
add_option( "staticlibpath", "comma separated list of dirs to search for staticlib arguments" , 1 , True )

add_option( "boost-compiler", "compiler used for boost (gcc41)" , 1 , True , "boostCompiler" )
add_option( "boost-version", "boost version for linking(1_38)" , 1 , True , "boostVersion" )

add_option( "no-glibc-check" , "don't check for new versions of glibc" , 0 , False )

# experimental features
add_option( "mm", "use main memory instead of memory mapped files" , 0 , True )
add_option( "asio" , "Use Asynchronous IO (NOT READY YET)" , 0 , True )
add_option( "ssl" , "Enable SSL" , 0 , True )

# library choices
add_option( "usesm" , "use spider monkey for javascript" , 0 , True )
add_option( "usev8" , "use v8 for javascript" , 0 , True )

# mongo feature options
add_option( "noshell", "don't build shell" , 0 , True )
add_option( "safeshell", "don't let shell scripts run programs (still, don't run untrusted scripts)" , 0 , True )
add_option( "win2008plus", "use newer operating system API features" , 0 , False )

# dev optoins
add_option( "d", "debug build no optimization, etc..." , 0 , True , "debugBuild" )
add_option( "dd", "debug build no optimization, additional debug logging, etc..." , 0 , False , "debugBuildAndLogging" )
add_option( "durableDefaultOn" , "have durable default to on" , 0 , True )
add_option( "durableDefaultOff" , "have durable default to off" , 0 , True )

add_option( "pch" , "use precompiled headers to speed up the build (experimental)" , 0 , True , "usePCH" )
add_option( "distcc" , "use distcc for distributing builds" , 0 , False )
add_option( "clang" , "use clang++ rather than g++ (experimental)" , 0 , True )

# debugging/profiling help

# to use CPUPROFILE=/tmp/profile
# to view pprof -gv mongod /tmp/profile
add_option( "pg", "link against profiler" , 0 , False , "profile" )
add_option( "tcmalloc" , "link against tcmalloc" , 0 , False )
add_option( "gdbserver" , "build in gdb server support" , 0 , True )
add_option( "heapcheck", "link to heap-checking malloc-lib and look for memory leaks during tests" , 0 , False )

add_option("smokedbprefix", "prefix to dbpath et al. for smoke tests", 1 , False )

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
linux = False
linux64  = False
darwin = False
windows = False
freebsd = False
openbsd = False
solaris = False
force64 = has_option( "force64" )
if not force64 and os.getcwd().endswith( "mongo-64" ):
    force64 = True
    print( "*** assuming you want a 64-bit build b/c of directory *** " )
msarch = None
if force64:
    msarch = "amd64"

force32 = has_option( "force32" ) 
release = has_option( "release" )
static = has_option( "static" )

debugBuild = has_option( "debugBuild" ) or has_option( "debugBuildAndLogging" ) 
debugLogging = has_option( "debugBuildAndLogging" )
noshell = has_option( "noshell" ) 

usesm = has_option( "usesm" )
usev8 = has_option( "usev8" ) 

asio = has_option( "asio" )

usePCH = has_option( "usePCH" )

justClientLib = (COMMAND_LINE_TARGETS == ['mongoclient'])

env = Environment( MSVS_ARCH=msarch , tools = ["default", "gch"], toolpath = '.' )
if has_option( "cxx" ):
    env["CC"] = get_option( "cxx" )
    env["CXX"] = get_option( "cxx" )
elif has_option("clang"):
    env["CC"] = 'clang'
    env["CXX"] = 'clang++'

if has_option( "cc" ):
    env["CC"] = get_option( "cc" )

env["LIBPATH"] = []

if has_option( "libpath" ):
    env["LIBPATH"] = [get_option( "libpath" )]

if has_option( "cpppath" ):
    env["CPPPATH"] = [get_option( "cpppath" )]

env.Append( CPPDEFINES=[ "_SCONS" , "MONGO_EXPOSE_MACROS" ] )
env.Append( CPPPATH=[ "." ] )

if has_option( "safeshell" ):
    env.Append( CPPDEFINES=[ "MONGO_SAFE_SHELL" ] )

if has_option( "durableDefaultOn" ):
    env.Append( CPPDEFINES=[ "_DURABLEDEFAULTON" ] )

if has_option( "durableDefaultOff" ):
    env.Append( CPPDEFINES=[ "_DURABLEDEFAULTOFF" ] )

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

if ( not ( usesm or usev8 or justClientLib) ):
    usesm = True
    options_topass["usesm"] = True

distBuild = len( COMMAND_LINE_TARGETS ) == 1 and ( str( COMMAND_LINE_TARGETS[0] ) == "s3dist" or str( COMMAND_LINE_TARGETS[0] ) == "dist" )

extraLibPlaces = []

def addExtraLibs( s ):
    for x in s.split(","):
        env.Append( CPPPATH=[ x + "/include" ] )
        env.Append( LIBPATH=[ x + "/lib" ] )
        env.Append( LIBPATH=[ x + "/lib64" ] )
        extraLibPlaces.append( x + "/lib" )

if has_option( "extrapath" ):
    addExtraLibs( GetOption( "extrapath" ) )
    release = True # this is so we force using .a

if has_option( "extrapathdyn" ):
    addExtraLibs( GetOption( "extrapathdyn" ) )

if has_option( "extralib" ):
    for x in GetOption( "extralib" ).split( "," ):
        env.Append( LIBS=[ x ] )

class InstallSetup:
    binaries = False
    clientSrc = False
    headers = False
    bannerDir = None
    headerRoot = "include"

    def __init__(self):
        self.default()
    
    def default(self):
        self.binaries = True
        self.libraries = False
        self.clientSrc = False
        self.headers = False
        self.bannerDir = None
        self.headerRoot = "include"
        self.clientTestsDir = None

    def justClient(self):
        self.binaries = False
        self.libraries = False
        self.clientSrc = True
        self.headers = True
        self.bannerDir = "distsrc/client/"
        self.headerRoot = ""
        self.clientTestsDir = "client/examples/"
        
installSetup = InstallSetup()
if distBuild:
    installSetup.bannerDir = "distsrc"

if has_option( "full" ):
    installSetup.headers = True
    installSetup.libraries = True


# ------    SOURCE FILE SETUP -----------

commonFiles = Split( "pch.cpp buildinfo.cpp db/indexkey.cpp db/jsobj.cpp bson/oid.cpp db/json.cpp db/lasterror.cpp db/nonce.cpp db/queryutil.cpp db/querypattern.cpp db/projection.cpp shell/mongo.cpp" )
commonFiles += [ "util/background.cpp" , "util/util.cpp" , "util/file_allocator.cpp" ,
                 "util/assert_util.cpp" , "util/log.cpp" , "util/ramlog.cpp" , "util/md5main.cpp" , "util/base64.cpp", "util/concurrency/vars.cpp", "util/concurrency/task.cpp", "util/debug_util.cpp",
                 "util/concurrency/thread_pool.cpp", "util/password.cpp", "util/version.cpp", "util/signal_handlers.cpp",  
                 "util/histogram.cpp", "util/concurrency/spin_lock.cpp", "util/text.cpp" , "util/stringutils.cpp" ,
                 "util/concurrency/synchronization.cpp" ]
commonFiles += [ "util/net/sock.cpp" , "util/net/httpclient.cpp" , "util/net/message.cpp" , "util/net/message_port.cpp" , "util/net/listen.cpp" ]
commonFiles += Glob( "util/*.c" ) 
commonFiles += Split( "client/connpool.cpp client/dbclient.cpp client/dbclient_rs.cpp client/dbclientcursor.cpp client/model.cpp client/syncclusterconnection.cpp client/distlock.cpp s/shardconnection.cpp" )

#mmap stuff

coreDbFiles = [ "db/commands.cpp" ]
coreServerFiles = [ "util/net/message_server_port.cpp" , 
                    "client/parallel.cpp" , "db/common.cpp", 
                    "util/net/miniwebserver.cpp" , "db/dbwebserver.cpp" , 
                    "db/matcher.cpp" , "db/dbcommands_generic.cpp" , "db/commands/cloud.cpp", "db/dbmessage.cpp" ]

mmapFiles = [ "util/mmap.cpp" ]

if has_option( "mm" ):
    mmapFiles += [ "util/mmap_mm.cpp" ]
elif os.sys.platform == "win32":
    mmapFiles += [ "util/mmap_win.cpp" ]
else:
    mmapFiles += [ "util/mmap_posix.cpp" ]

coreServerFiles += mmapFiles

processInfoFiles = [ "util/processinfo.cpp" ]

if os.path.exists( "util/processinfo_" + os.sys.platform + ".cpp" ):
    processInfoFiles += [ "util/processinfo_" + os.sys.platform + ".cpp" ]
elif os.sys.platform == "linux3":
    processInfoFiles += [ "util/processinfo_linux2.cpp" ]
else:
    processInfoFiles += [ "util/processinfo_none.cpp" ]

coreServerFiles += processInfoFiles

if has_option( "asio" ):
    coreServerFiles += [ "util/net/message_server_asio.cpp" ]

# mongod files - also files used in tools. present in dbtests, but not in mongos and not in client libs.
serverOnlyFiles = Split( "util/compress.cpp db/key.cpp db/btreebuilder.cpp util/logfile.cpp util/alignedbuilder.cpp db/mongommf.cpp db/dur.cpp db/durop.cpp db/dur_writetodatafiles.cpp db/dur_preplogbuffer.cpp db/dur_commitjob.cpp db/dur_recover.cpp db/dur_journal.cpp db/introspect.cpp db/btree.cpp db/clientcursor.cpp db/tests.cpp db/repl.cpp db/repl/rs.cpp db/repl/consensus.cpp db/repl/rs_initiate.cpp db/repl/replset_commands.cpp db/repl/manager.cpp db/repl/health.cpp db/repl/heartbeat.cpp db/repl/rs_config.cpp db/repl/rs_rollback.cpp db/repl/rs_sync.cpp db/repl/rs_initialsync.cpp db/oplog.cpp db/repl_block.cpp db/btreecursor.cpp db/cloner.cpp db/namespace.cpp db/cap.cpp db/matcher_covered.cpp db/dbeval.cpp db/restapi.cpp db/dbhelpers.cpp db/instance.cpp db/client.cpp db/database.cpp db/pdfile.cpp db/record.cpp db/cursor.cpp db/security.cpp db/queryoptimizer.cpp db/queryoptimizercursor.cpp db/extsort.cpp db/cmdline.cpp" )

serverOnlyFiles += [ "db/index.cpp" , "db/scanandorder.cpp" ] + Glob( "db/geo/*.cpp" ) + Glob( "db/ops/*.cpp" )

serverOnlyFiles += [ "db/dbcommands.cpp" , "db/dbcommands_admin.cpp" ]
serverOnlyFiles += [ "db/commands/%s.cpp" % x for x in ["distinct","find_and_modify","group","mr"] ]
serverOnlyFiles += [ "db/driverHelpers.cpp" ]

coreServerFiles += Glob( "db/stats/*.cpp" )
coreServerFiles += [ "db/commands/isself.cpp", "db/security_common.cpp", "db/security_commands.cpp" ]

scriptingFiles = [ "scripting/engine.cpp" , "scripting/utils.cpp" , "scripting/bench.cpp" ]

if usesm:
    scriptingFiles += [ "scripting/engine_spidermonkey.cpp" ]
elif usev8:
    scriptingFiles += [ Glob( "scripting/*v8*.cpp" ) ]
else:
    scriptingFiles += [ "scripting/engine_none.cpp" ]

coreShardFiles = [ "s/config.cpp" , "s/grid.cpp" , "s/chunk.cpp" , "s/shard.cpp" , "s/shardkey.cpp" ]
shardServerFiles = coreShardFiles + Glob( "s/strategy*.cpp" ) + [ "s/commands_admin.cpp" , "s/commands_public.cpp" , "s/request.cpp" , "s/client.cpp" , "s/cursors.cpp" ,  "s/server.cpp" , "s/config_migrate.cpp" , "s/s_only.cpp" , "s/stats.cpp" , "s/balance.cpp" , "s/balancer_policy.cpp" , "db/cmdline.cpp" , "s/writeback_listener.cpp" , "s/shard_version.cpp", "s/mr_shard.cpp", "s/security.cpp" ]
serverOnlyFiles += coreShardFiles + [ "s/d_logic.cpp" , "s/d_writeback.cpp" , "s/d_migrate.cpp" , "s/d_state.cpp" , "s/d_split.cpp" , "client/distlock_test.cpp" , "s/d_chunk_manager.cpp" ]

serverOnlyFiles += [ "db/module.cpp" ] + Glob( "db/modules/*.cpp" )

modules = []
moduleNames = []

for x in os.listdir( "db/modules/" ):
    if x.find( "." ) >= 0:
        continue
    print( "adding module: " + x )
    moduleNames.append( x )
    modRoot = "db/modules/" + x + "/"

    modBuildFile = modRoot + "build.py"
    myModule = None
    if os.path.exists( modBuildFile ):
        myModule = imp.load_module( "module_" + x , open( modBuildFile , "r" ) , modBuildFile , ( ".py" , "r" , imp.PY_SOURCE  ) )
        modules.append( myModule )
        
    if myModule and "customIncludes" in dir(myModule) and myModule.customIncludes:
        pass
    else:
        serverOnlyFiles += Glob( modRoot + "src/*.cpp" )


allClientFiles = commonFiles + coreDbFiles + [ "client/clientOnly.cpp" , "client/gridfs.cpp" ];

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

DEFAULT_INSTALL_DIR = "/usr/local"
installDir = DEFAULT_INSTALL_DIR
nixLibPrefix = "lib"

distName = GetOption( "distname" )
dontReplacePackage = False

if distBuild:
    release = True

def isDriverBuild():
    return GetOption( "prefix" ) and GetOption( "prefix" ).find( "mongo-cxx-driver" ) >= 0

if has_option( "prefix" ):
    installDir = GetOption( "prefix" )
    if isDriverBuild():
        installSetup.justClient()


def findVersion( root , choices ):
    if not isinstance(root, list):
        root = [root]
    for r in root:
        for c in choices:
            if ( os.path.exists( r + c ) ):
                return r + c
    raise RuntimeError("can't find a version of [" + repr(root) + "] choices: " + repr(choices))

def choosePathExist( choices , default=None):
    for c in choices:
        if c != None and os.path.exists( c ):
            return c
    return default

def filterExists(paths):
    return filter(os.path.exists, paths)

if "darwin" == os.sys.platform:
    darwin = True
    platform = "osx" # prettier than darwin

    if env["CXX"] is None:
        print( "YO" )
        if os.path.exists( "/usr/bin/g++-4.2" ):
            env["CXX"] = "g++-4.2"

    nix = True

    if force64:
        env.Append( CPPPATH=["/usr/64/include"] )
        env.Append( LIBPATH=["/usr/64/lib"] )
        if installDir == DEFAULT_INSTALL_DIR and not distBuild:
            installDir = "/usr/64/"
    else:
        env.Append( CPPPATH=filterExists(["/sw/include" , "/opt/local/include"]) )
        env.Append( LIBPATH=filterExists(["/sw/lib/", "/opt/local/lib"]) )

elif os.sys.platform.startswith("linux"):
    linux = True
    platform = "linux"

    if os.uname()[4] == "x86_64" and not force32:
        linux64 = True
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
     env.Append( CPPDEFINES=[ "__sunos__" ] )
     env.Append( LIBS=["socket","resolv"] )

elif os.sys.platform.startswith( "freebsd" ):
    nix = True
    freebsd = True
    env.Append( CPPPATH=[ "/usr/local/include" ] )
    env.Append( LIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__freebsd__" ] )

elif os.sys.platform.startswith( "openbsd" ):
    nix = True
    openbsd = True
    env.Append( CPPPATH=[ "/usr/local/include" ] )
    env.Append( LIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__openbsd__" ] )

elif "win32" == os.sys.platform:
    windows = True
    #if force64:
    #    release = True

    if has_option( "win2008plus" ):
        env.Append( CPPDEFINES=[ "MONGO_USE_SRW_ON_WINDOWS" ] )

    for pathdir in env['ENV']['PATH'].split(os.pathsep):
	if os.path.exists(os.path.join(pathdir, 'cl.exe')):
            print( "found visual studio at " + pathdir )
	    break
    else:
	#use current environment
	env['ENV'] = dict(os.environ)

    def find_boost():
        for x in ('', ' (x86)'):	
            boostDir = "C:/Program Files" + x + "/boost/latest"
            if os.path.exists( boostDir ):
                return boostDir
            for bv in reversed( range(33,50) ):
	            for extra in ('', '_0', '_1'):
		            boostDir = "C:/Program Files" + x + "/Boost/boost_1_" + str(bv) + extra
		            if os.path.exists( boostDir ):
		                return boostDir
        if os.path.exists( "C:/boost" ):
	        return "C:/boost"
        if os.path.exists( "/boost" ):
	        return "/boost"
        return None

    boostDir = find_boost()
    if boostDir is None:
        print( "can't find boost" )
        Exit(1)
    else:
        print( "boost found at '" + boostDir + "'" )

    boostLibs = []

    env.Append( CPPDEFINES=[ "_UNICODE" ] )
    env.Append( CPPDEFINES=[ "UNICODE" ] )

    winSDKHome = findVersion( [ "C:/Program Files/Microsoft SDKs/Windows/", "C:/Program Files (x86)/Microsoft SDKs/Windows/" ] ,
                              [ "v7.1", "v7.0A", "v7.0", "v6.1", "v6.0a", "v6.0" ] )
    print( "Windows SDK Root '" + winSDKHome + "'" )

    env.Append( CPPPATH=[ boostDir , winSDKHome + "/Include" ] )

    # consider adding /MP build with multiple processes option.

    # /EHsc exception handling style for visual studio
    # /W3 warning level
    env.Append( CPPFLAGS=" /EHsc /W3 " )

    # some warnings we don't like:
    env.Append( CPPFLAGS=" /wd4355 /wd4800 /wd4267 /wd4244 " )
    
    # PSAPI_VERSION relates to process api dll Psapi.dll.
    env.Append( CPPDEFINES=["_CONSOLE","_CRT_SECURE_NO_WARNINGS","PSAPI_VERSION=1" ] )

    # this would be for pre-compiled headers, could play with it later  
    #env.Append( CPPFLAGS=' /Yu"pch.h" ' ) 

    # docs say don't use /FD from command line (minimal rebuild)
    # /Gy function level linking
    # /Gm is minimal rebuild, but may not work in parallel mode.
    if release:
        env.Append( CPPDEFINES=[ "NDEBUG" ] )
        env.Append( CPPFLAGS= " /O2 /Gy " )
        env.Append( CPPFLAGS= " /MT /Zi /TP /errorReport:none " )
        # TODO: this has caused some linking problems :
        # /GL whole program optimization
        # /LTCG link time code generation
        env.Append( CPPFLAGS= " /GL " ) 
        env.Append( LINKFLAGS=" /LTCG " )
    else:
        # /Od disable optimization
        # /ZI debug info w/edit & continue 
        # /TP it's a c++ file
        # RTC1 /GZ (Enable Stack Frame Run-Time Error Checking)
        env.Append( CPPFLAGS=" /RTC1 /MDd /Z7 /TP /errorReport:none " )
        env.Append( CPPFLAGS=' /Fd"mongod.pdb" ' )

        if debugBuild:
            env.Append( LINKFLAGS=" /debug " )
            env.Append( CPPFLAGS=" /Od " )
            
        if debugLogging:
            env.Append( CPPDEFINES=[ "_DEBUG" ] )

    if force64 and os.path.exists( boostDir + "/lib/vs2010_64" ):
        env.Append( LIBPATH=[ boostDir + "/lib/vs2010_64" ] )
    elif not force64 and os.path.exists( boostDir + "/lib/vs2010_32" ):
        env.Append( LIBPATH=[ boostDir + "/lib/vs2010_32" ] )
    else:
        env.Append( LIBPATH=[ boostDir + "/Lib" ] )

    if force64:
        env.Append( LIBPATH=[ winSDKHome + "/Lib/x64" ] )
    else:
        env.Append( LIBPATH=[ winSDKHome + "/Lib" ] )

    if release:
        #env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  /NODEFAULTLIB:MSVCRTD " )
        env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  " )
    else:
        env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  /NODEFAULTLIB:MSVCRT  " )

    winLibString = "ws2_32.lib kernel32.lib advapi32.lib Psapi.lib"

    if force64:
        
        winLibString += ""
        #winLibString += " LIBCMT LIBCPMT "

    else:
        winLibString += " user32.lib gdi32.lib winspool.lib comdlg32.lib  shell32.lib ole32.lib oleaut32.lib "
        winLibString += " odbc32.lib odbccp32.lib uuid.lib "

    env.Append( LIBS=Split(winLibString) )

    # dm these should automatically be defined by the compiler. commenting out to see if works. jun2010
    #if force64:
    #    env.Append( CPPDEFINES=["_AMD64_=1"] )
    #else:
    #    env.Append( CPPDEFINES=["_X86_=1"] )

    env.Append( CPPPATH=["../winpcap/Include"] )
    env.Append( LIBPATH=["../winpcap/Lib"] )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )

if nix:

    if has_option( "distcc" ):
        env["CXX"] = "distcc " + env["CXX"]
        
    # -Winvalid-pch Warn if a precompiled header (see Precompiled Headers) is found in the search path but can't be used. 
    env.Append( CPPFLAGS="-fPIC -fno-strict-aliasing -ggdb -pthread -Wall -Wsign-compare -Wno-unknown-pragmas -Winvalid-pch" )
    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    if linux:
        env.Append( CPPFLAGS=" -Werror -pipe " )
        if not has_option('clang'): 
            env.Append( CPPFLAGS=" -fno-builtin-memcmp " ) # glibc's memcmp is faster than gcc's

    env.Append( CPPDEFINES="_FILE_OFFSET_BITS=64" )
    env.Append( CXXFLAGS=" -Wnon-virtual-dtor " )
    env.Append( LINKFLAGS=" -fPIC -pthread -rdynamic" )
    env.Append( LIBS=[] )

    #make scons colorgcc friendly
    env['ENV']['HOME'] = os.environ['HOME']
    env['ENV']['TERM'] = os.environ['TERM']

    if linux and has_option( "sharedclient" ):
        env.Append( LINKFLAGS=" -Wl,--as-needed -Wl,-zdefs " )

    if debugBuild:
        env.Append( CPPFLAGS=" -O0 -fstack-protector " );
        env['ENV']['GLIBCXX_FORCE_NEW'] = 1; # play nice with valgrind
    else:
        env.Append( CPPFLAGS=" -O3 " )
        #env.Append( CPPFLAGS=" -fprofile-generate" )
        #env.Append( LINKFLAGS=" -fprofile-generate" )
        # then:
        #env.Append( CPPFLAGS=" -fprofile-use" )
        #env.Append( LINKFLAGS=" -fprofile-use" )        

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

    if has_option( "profile" ):
        env.Append( LIBS=[ "profiler" ] )

    if has_option( "gdbserver" ):
        env.Append( CPPDEFINES=["USE_GDBSERVER"] )

    # pre-compiled headers
    if usePCH and 'Gch' in dir( env ):
        print( "using precompiled headers" )
        if has_option('clang'):
            #env['GCHSUFFIX']  = '.pch' # clang++ uses pch.h.pch rather than pch.h.gch
            #env.Prepend( CXXFLAGS=' -include pch.h ' ) # clang++ only uses pch from command line
            print( "ERROR: clang pch is broken for now" )
            Exit(1)
        env['Gch'] = env.Gch( [ "pch.h" ] )[0]
        env['GchSh'] = env.GchSh( [ "pch.h" ] )[0]
    elif os.path.exists('pch.h.gch'):
        print( "removing precompiled headers" )
        os.unlink('pch.h.gch') # gcc uses the file if it exists

if usev8:
    env.Prepend( CPPPATH=["../v8/include/"] )
    env.Prepend( LIBPATH=["../v8/"] )

if "uname" in dir(os):
    hacks = buildscripts.findHacks( os.uname() )
    if hacks is not None:
        hacks.insert( env , { "linux64" : linux64 } )

if has_option( "ssl" ):
    env.Append( CPPDEFINES=["MONGO_SSL"] )
    env.Append( LIBS=["ssl"] )
    if darwin:
        env.Append( LIBS=["crypto"] )

try:
    umask = os.umask(022)
except OSError:
    pass

if not windows:
    for keysuffix in [ "1" , "2" ]:
        keyfile = "jstests/libs/key%s" % keysuffix
        os.chmod( keyfile , stat.S_IWUSR|stat.S_IRUSR )

moduleFiles = {}
for x in os.listdir( "third_party" ):
    if not x.endswith( ".py" ) or x.find( "#" ) >= 0:
        continue
         
    shortName = x.rpartition( "." )[0]
    path = "third_party/%s" % x


    myModule = imp.load_module( "third_party_%s" % shortName , open( path , "r" ) , path , ( ".py" , "r" , imp.PY_SOURCE ) )
    fileLists = { "commonFiles" : commonFiles , "serverOnlyFiles" : serverOnlyFiles , "scriptingFiles" : scriptingFiles, "moduleFiles" : moduleFiles }
    
    options_topass["windows"] = windows
    options_topass["nix"] = nix
    
    myModule.configure( env , fileLists , options_topass )

coreServerFiles += scriptingFiles

# --- check system ---

def getSysInfo():
    if windows:
        return "windows " + str( sys.getwindowsversion() )
    else:
        return " ".join( os.uname() )

def add_exe(target):
    if windows:
        return target + ".exe"
    return target

def setupBuildInfoFile( outFile ):
    version = utils.getGitVersion()
    if len(moduleNames) > 0:
        version = version + " modules: " + ','.join( moduleNames )
    sysInfo = getSysInfo()
    contents = '\n'.join([
        '#include "pch.h"',
        '#include <iostream>',
        '#include <boost/version.hpp>',
        'namespace mongo { const char * gitVersion(){ return "' + version + '"; } }',
        'namespace mongo { string sysInfo(){ return "' + sysInfo + ' BOOST_LIB_VERSION=" BOOST_LIB_VERSION ; } }',
        ])

    contents += '\n';

    if os.path.exists( outFile ) and open( outFile ).read().strip() == contents.strip():
        return

    contents += '\n';

    out = open( outFile , 'w' )
    out.write( contents )
    out.close()

setupBuildInfoFile( "buildinfo.cpp" )

def bigLibString( myenv ):
    s = str( myenv["LIBS"] )
    if 'SLIBS' in myenv._dict:
        s += str( myenv["SLIBS"] )
    return s


def doConfigure( myenv , shell=False ):
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

    def myCheckLib( poss , failIfNotFound=False , staticOnly=False):

        if type( poss ) != types.ListType :
            poss = [poss]

        allPlaces = [];
        allPlaces += extraLibPlaces
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


        if release and not windows and failIfNotFound:
            print( "ERROR: can't find static version of: " + str( poss ) + " in: " + str( allPlaces ) )
            Exit(1)

        res = not staticOnly and conf.CheckLib( poss )
        if res:
            return True

        if failIfNotFound:
            print( "can't find or link against library " + str( poss ) + " in " + str( myenv["LIBPATH"] ) )
            print( "see config.log for more information" )
            if windows:
                print( "use scons --64 when cl.exe is 64 bit compiler" )
            Exit(1)

        return False

    if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
        print( "can't find boost headers" )
        if shell:
            print( "\tshell might not compile" )
        else:
            Exit(1)

    if asio:
        if conf.CheckCXXHeader( "boost/asio.hpp" ):
            myenv.Append( CPPDEFINES=[ "USE_ASIO" ] )
        else:
            print( "WARNING: old version of boost - you should consider upgrading" )

    # this will add it if it exists and works
    myCheckLib( [ "boost_system" + boostCompiler + "-mt" + boostVersion ,
                  "boost_system" + boostCompiler + boostVersion ] )

    for b in boostLibs:
        l = "boost_" + b
        myCheckLib( [ l + boostCompiler + "-mt" + boostVersion ,
                      l + boostCompiler + boostVersion ] ,
                    release or not shell)

    if not conf.CheckCXXHeader( "execinfo.h" ):
        myenv.Append( CPPDEFINES=[ "NOEXECINFO" ] )

    myenv["_HAVEPCAP"] = myCheckLib( ["pcap", "wpcap"] )
    removeIfInList( myenv["LIBS"] , "pcap" )
    removeIfInList( myenv["LIBS"] , "wpcap" )

    for m in modules:
        if "customIncludes" in dir(m) and m.customIncludes:
            m.configure( conf , myenv , serverOnlyFiles )
        else:
            m.configure( conf , myenv )

    if solaris:
        conf.CheckLib( "nsl" )

    if usev8:
        if debugBuild:
            myCheckLib( [ "v8_g" , "v8" ] , True )
        else:
            myCheckLib( "v8" , True )

    # requires ports devel/libexecinfo to be installed
    if freebsd or openbsd:
        myCheckLib( "execinfo", True )
        env.Append( LIBS=[ "execinfo" ] )

    # Handle staticlib,staticlibpath options.
    staticlibfiles = []
    if has_option( "staticlib" ):
        # FIXME: probably this loop ought to do something clever
        # depending on whether we want to use 32bit or 64bit
        # libraries.  For now, we sort of rely on the user supplying a
        # sensible staticlibpath option. (myCheckLib implements an
        # analogous search, but it also does other things I don't
        # understand, so I'm not using it.)
        if has_option ( "staticlibpath" ):
            dirs = GetOption ( "staticlibpath" ).split( "," )
        else:
            dirs = [ "/usr/lib64", "/usr/lib" ]

        for l in GetOption( "staticlib" ).split( "," ):
            removeIfInList(myenv["LIBS"], l)
            found = False
            for d in dirs:
                f=  "%s/lib%s.a" % ( d, l )
                if os.path.exists( f ):
                    staticlibfiles.append(f)
                    found = True
                    break
            if not found:
                raise RuntimeError("can't find a static %s" % l)

    # 'tcmalloc' needs to be the last library linked. Please, add new libraries before this 
    # point.
    if has_option( "heapcheck" ) and not shell:
        if ( not debugBuild ) and ( not debugLogging ):
            print( "--heapcheck needs --d or --dd" )
            Exit( 1 )

        if not conf.CheckCXXHeader( "google/heap-checker.h" ):
            print( "--heapcheck neads header 'google/heap-checker.h'" )
            Exit( 1 )

        myCheckLib( "tcmalloc" , True );  # if successful, appedded 'tcmalloc' to myenv[ LIBS ]
        myenv.Append( CPPDEFINES=[ "HEAP_CHECKING" ] )
        myenv.Append( CPPFLAGS="-fno-omit-frame-pointer" )

    # FIXME doConfigure() is being called twice, in the case of the shell. So if it is called 
    # with shell==True, it'd be on its second call and it would need to rearrange the libraries'
    # order. The following removes tcmalloc from the LIB's list and reinserts it at the end.
    if has_option( "heapcheck" ) and shell:
        removeIfInList( myenv["LIBS"] , "tcmalloc" )
        myenv.Append( LIBS="tcmalloc" )

    myenv.Append(LINKCOM=" $STATICFILES")
    myenv.Append(STATICFILES=staticlibfiles)

    if has_option( "tcmalloc" ):
        myCheckLib( "tcmalloc" , True );  # if successful, appedded 'tcmalloc' to myenv[ LIBS ]


    return conf.Finish()

env = doConfigure( env )


# --- jsh ---

def jsToH(target, source, env):

    outFile = str( target[0] )

    h =  ['#include "bson/stringdata.h"'
         ,'namespace mongo {'
         ,'struct JSFile{ const char* name; const StringData& source; };'
         ,'namespace JSFiles{'
         ]

    def cppEscape(s):
        s = s.strip()
        s = s.replace( '\\' , '\\\\' )
        s = s.replace( '"' , r'\"' )
        return s

    for s in source:
        filename = str(s)
        objname = os.path.split(filename)[1].split('.')[0]
        stringname = '_jscode_raw_' + objname

        h.append('const StringData ' + stringname + " = ")

        for l in open( filename , 'r' ):
            h.append( '"' + cppEscape(l) + r'\n" ' )

        h.append(";")
        h.append('extern const JSFile %s;'%objname) #symbols aren't exported w/o this
        h.append('const JSFile %s = { "%s" , %s };'%(objname, filename.replace('\\', '/'), stringname))

    h.append("} // namespace JSFiles")
    h.append("} // namespace mongo")
    h.append("")

    text = '\n'.join(h);

    out = open( outFile , 'wb' )
    out.write( text )
    out.close()

    # mongo_vstudio.cpp is in git as the .vcproj doesn't generate this file.
    if outFile.find( "mongo.cpp" ) >= 0:
        out = open( outFile.replace( "mongo" , "mongo_vstudio" ) , 'wb' )
        out.write( text )
        out.close()

    return None

jshBuilder = Builder(action = jsToH,
                    suffix = '.cpp',
                    src_suffix = '.js')

env.Append( BUILDERS={'JSHeader' : jshBuilder})


# --- targets ----

# profile guided
#if windows:
#    if release:
#        env.Append( LINKFLAGS="/PGD:test.pgd" )
#        env.Append( LINKFLAGS="/LTCG:PGINSTRUMENT" )
#        env.Append( LINKFLAGS="/LTCG:PGOPTIMIZE" )

testEnv = env.Clone()
testEnv.Append( CPPPATH=["../"] )
testEnv.Prepend( LIBS=[ "mongotestfiles" ] )
testEnv.Prepend( LIBPATH=["."] )

# ----- TARGETS ------

def checkErrorCodes():
    import buildscripts.errorcodes as x
    if x.checkErrorCodes() == False:
        print( "next id to use:" + str( x.getNextCode() ) )
        Exit(-1)

checkErrorCodes()

# main db target
mongodOnlyFiles = [ "db/db.cpp", "db/compact.cpp" ]
if windows:
    mongodOnlyFiles.append( "util/ntservice.cpp" ) 
mongod = env.Program( "mongod" , commonFiles + coreDbFiles + coreServerFiles + serverOnlyFiles + mongodOnlyFiles )
Default( mongod )

# tools
allToolFiles = commonFiles + coreDbFiles + coreServerFiles + serverOnlyFiles + [ "client/gridfs.cpp", "tools/tool.cpp" ]
normalTools = [ "dump" , "restore" , "export" , "import" , "files" , "stat" , "top" , "oplog" ]
env.Alias( "tools" , [ add_exe( "mongo" + x ) for x in normalTools ] )
for x in normalTools:
    env.Program( "mongo" + x , allToolFiles + [ "tools/" + x + ".cpp" ] )

#some special tools
env.Program( "bsondump" , allToolFiles + [ "tools/bsondump.cpp" ] )
env.Program( "mongobridge" , allToolFiles + [ "tools/bridge.cpp" ] )
#env.Program( "mongoperf" , allToolFiles + [ "client/examples/mongoperf.cpp" ] )

# mongos
mongos = env.Program( "mongos" , commonFiles + coreDbFiles + coreServerFiles + shardServerFiles )

# c++ library
clientLib = env.Library( "mongoclient" , allClientFiles )
clientLibName = str( clientLib[0] )
if has_option( "sharedclient" ):
    sharedClientLibName = str( env.SharedLibrary( "mongoclient" , allClientFiles )[0] )
env.Library( "mongotestfiles" , commonFiles + coreDbFiles + coreServerFiles + serverOnlyFiles + ["client/gridfs.cpp"])
env.Library( "mongoshellfiles" , allClientFiles + coreServerFiles )

clientEnv = env.Clone();
clientEnv.Append( CPPPATH=["../"] )
clientEnv.Prepend( LIBS=[ clientLib ] )
clientEnv.Prepend( LIBPATH=["."] )
clientEnv["CPPDEFINES"].remove( "MONGO_EXPOSE_MACROS" )
l = clientEnv[ "LIBS" ]

clientTests = []

# examples
clientTests += [ clientEnv.Program( "firstExample" , [ "client/examples/first.cpp" ] ) ]
clientTests += [ clientEnv.Program( "rsExample" , [ "client/examples/rs.cpp" ] ) ]
clientTests += [ clientEnv.Program( "secondExample" , [ "client/examples/second.cpp" ] ) ]
clientTests += [ clientEnv.Program( "whereExample" , [ "client/examples/whereExample.cpp" ] ) ]
clientTests += [ clientEnv.Program( "authTest" , [ "client/examples/authTest.cpp" ] ) ]
clientTests += [ clientEnv.Program( "httpClientTest" , [ "client/examples/httpClientTest.cpp" ] ) ]
clientTests += [ clientEnv.Program( "bsondemo" , [ "bson/bsondemo/bsondemo.cpp" ] ) ]

# dbtests test binary
test = testEnv.Program( "test" , Glob( "dbtests/*.cpp" ) )
if windows:
    testEnv.Alias( "test" , "test.exe" )
perftest = testEnv.Program( "perftest", [ "dbtests/framework.cpp" , "dbtests/perf/perftest.cpp" ] )
clientTests += [ clientEnv.Program( "clientTest" , [ "client/examples/clientTest.cpp" ] ) ]

# --- sniffer ---
mongosniff_built = False
if darwin or clientEnv["_HAVEPCAP"]:
    mongosniff_built = True
    sniffEnv = env.Clone()
    sniffEnv.Append( CPPDEFINES="MONGO_EXPOSE_MACROS" )

    if not windows:
        sniffEnv.Append( LIBS=[ "pcap" ] )
    else:
        sniffEnv.Append( LIBS=[ "wpcap" ] )

    sniffEnv.Prepend( LIBPATH=["."] )
    sniffEnv.Prepend( LIBS=[ "mongotestfiles" ] )

    sniffEnv.Program( "mongosniff" , "tools/sniffer.cpp" )

# --- shell ---

# note, if you add a file here, need to add in engine.cpp currently
env.JSHeader( "shell/mongo.cpp"  , Glob( "shell/utils*.js" ) + [ "shell/db.js","shell/mongo.js","shell/mr.js","shell/query.js","shell/collection.js"] )

env.JSHeader( "shell/mongo-server.cpp"  , [ "shell/servers.js"] )

shellEnv = env.Clone();

if release and ( ( darwin and force64 ) or linux64 ):
    shellEnv["LINKFLAGS"] = env["LINKFLAGS_CLEAN"]
    shellEnv["LIBS"] = env["LIBS_CLEAN"]
    shellEnv["SLIBS"] = ""

if noshell:
    print( "not building shell" )
elif not onlyServer:
    l = shellEnv["LIBS"]

    if windows:
        shellEnv.Append( LIBS=["winmm.lib"] )

    coreShellFiles = [ "shell/dbshell.cpp" , "shell/shell_utils.cpp" , "shell/mongo-server.cpp" ]

    coreShellFiles.append( "third_party/linenoise/linenoise.cpp" )

    shellEnv.Prepend( LIBPATH=[ "." ] )
        
    shellEnv = doConfigure( shellEnv , shell=True )

    shellEnv.Prepend( LIBS=[ "mongoshellfiles"] )
    mongo = shellEnv.Program( "mongo" , moduleFiles["pcre"] + coreShellFiles )


#  ---- RUNNING TESTS ----

smokeEnv = testEnv.Clone()
smokeEnv['ENV']['PATH']=os.environ['PATH']
smokeEnv.Alias( "dummySmokeSideEffect", [], [] )

smokeFlags = []

# Ugh.  Frobbing the smokeFlags must precede using them to construct
# actions, I think.
if has_option( 'smokedbprefix'):
    smokeFlags += ['--smoke-db-prefix', GetOption( 'smokedbprefix')]

if 'startMongodSmallOplog' in COMMAND_LINE_TARGETS:
    smokeFlags += ["--small-oplog"]

def addTest(name, deps, actions):
    smokeEnv.Alias( name, deps, actions )
    smokeEnv.AlwaysBuild( name )
    # Prevent smoke tests from running in parallel
    smokeEnv.SideEffect( "dummySmokeSideEffect", name )

def addSmoketest( name, deps ):
    # Convert from smoke to test, smokeJs to js, and foo to foo
    target = name
    if name.startswith("smoke"):
        if name == "smoke":
            target = "test"
        else:
            target = name[5].lower() + name[6:]

    addTest(name, deps, [ "python buildscripts/smoke.py " + " ".join(smokeFlags) + ' ' + target ])

addSmoketest( "smoke", [ add_exe( "test" ) ] )
addSmoketest( "smokePerf", [ "perftest" ]  )
addSmoketest( "smokeClient" , clientTests )
addSmoketest( "mongosTest" , [ mongos[0].abspath ] )

# These tests require the mongo shell
if not onlyServer and not noshell:
    addSmoketest( "smokeJs", [add_exe("mongo")] )
    addSmoketest( "smokeClone", [ "mongo", "mongod" ] )
    addSmoketest( "smokeRepl", [ "mongo", "mongod", "mongobridge" ] )
    addSmoketest( "smokeReplSets", [ "mongo", "mongod", "mongobridge" ] )
    addSmoketest( "smokeDur", [ add_exe( "mongo" ) , add_exe( "mongod" ) , add_exe('mongorestore') ] )
    addSmoketest( "smokeDisk", [ add_exe( "mongo" ), add_exe( "mongod" ), add_exe( "mongodump" ), add_exe( "mongorestore" ) ] )
    addSmoketest( "smokeAuth", [ add_exe( "mongo" ), add_exe( "mongod" ) ] )
    addSmoketest( "smokeParallel", [ add_exe( "mongo" ), add_exe( "mongod" ) ] )
    addSmoketest( "smokeSharding", [ "mongo", "mongod", "mongos" ] )
    addSmoketest( "smokeJsPerf", [ "mongo" ] )
    addSmoketest( "smokeJsSlowNightly", [add_exe("mongo")])
    addSmoketest( "smokeJsSlowWeekly", [add_exe("mongo")])
    addSmoketest( "smokeQuota", [ "mongo" ] )
    addSmoketest( "smokeTool", [ add_exe( "mongo" ), add_exe("mongod"), "tools" ] )

# Note: although the test running logic has been moved to
# buildscripts/smoke.py, the interface to running the tests has been
# something like 'scons startMongod <suite>'; startMongod is now a
# no-op, and should go away eventually.
smokeEnv.Alias( "startMongod", [add_exe("mongod")]);
smokeEnv.AlwaysBuild( "startMongod" );
smokeEnv.SideEffect( "dummySmokeSideEffect", "startMongod" )

smokeEnv.Alias( "startMongodSmallOplog", [add_exe("mongod")], [] );
smokeEnv.AlwaysBuild( "startMongodSmallOplog" );
smokeEnv.SideEffect( "dummySmokeSideEffect", "startMongodSmallOplog" )

def addMongodReqTargets( env, target, source ):
    mongodReqTargets = [ "smokeClient", "smokeJs" ]
    for target in mongodReqTargets:
        smokeEnv.Depends( target, "startMongod" )
        smokeEnv.Depends( "smokeAll", target )

smokeEnv.Alias( "addMongodReqTargets", [], [addMongodReqTargets] )
smokeEnv.AlwaysBuild( "addMongodReqTargets" )

smokeEnv.Alias( "smokeAll", [ "smoke", "mongosTest", "smokeClone", "smokeRepl", "addMongodReqTargets", "smokeDisk", "smokeAuth", "smokeSharding", "smokeTool" ] )
smokeEnv.AlwaysBuild( "smokeAll" )

def addMongodReqNoJsTargets( env, target, source ):
    mongodReqTargets = [ "smokeClient" ]
    for target in mongodReqTargets:
        smokeEnv.Depends( target, "startMongod" )
        smokeEnv.Depends( "smokeAllNoJs", target )

smokeEnv.Alias( "addMongodReqNoJsTargets", [], [addMongodReqNoJsTargets] )
smokeEnv.AlwaysBuild( "addMongodReqNoJsTargets" )

smokeEnv.Alias( "smokeAllNoJs", [ "smoke", "mongosTest", "addMongodReqNoJsTargets" ] )
smokeEnv.AlwaysBuild( "smokeAllNoJs" )

def recordPerformance( env, target, source ):
    from buildscripts import benchmark_tools
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
        sub[ "trial" ][ "server_hash" ] = utils.getGitVersion()
        sub[ "trial" ][ "client_hash" ] = ""
        sub[ "trial" ][ "result" ] = val
        try:
            print(benchmark_tools.post_data(sub))
        except:
            print( "exception posting perf results" )
            print( sys.exc_info() )
    return False

addTest( "recordPerf", [ "perftest" ] , [ recordPerformance ] )

def run_shell_tests(env, target, source):
    from buildscripts import test_shell
    test_shell.mongo_path = windows and "mongo.exe" or "mongo"
    test_shell.run_tests()

env.Alias("test_shell", [], [run_shell_tests])
env.AlwaysBuild("test_shell")

#  ---- Docs ----
def build_docs(env, target, source):
    from buildscripts import docs
    docs.main()

env.Alias("docs", [], [build_docs])
env.AlwaysBuild("docs")

#  ---- astyle ----

def doStyling( env , target , source ):
    
    res = utils.execsys( "astyle --version" )
    res = " ".join(res)
    if res.count( "2." ) == 0:
        print( "astyle 2.x needed, found:" + res )
        Exit(-1)

    files = utils.getAllSourceFiles() 
    files = filter( lambda x: not x.endswith( ".c" ) , files )
    files.remove( "./shell/mongo_vstudio.cpp" )

    cmd = "astyle --options=mongo_astyle " + " ".join( files )
    res = utils.execsys( cmd )
    print( res[0] )
    print( res[1] )


env.Alias( "style" , [] , [ doStyling ] )
env.AlwaysBuild( "style" )



#  ----  INSTALL -------

def getSystemInstallName():
    n = platform + "-" + processor
    if static:
        n += "-static"
    if has_option("nostrip"):
        n += "-debugsymbols"
    if nix and os.uname()[2].startswith( "8." ):
        n += "-tiger"
        
    if len(moduleNames) > 0:
        n += "-" + "-".join( moduleNames )

    try:
        findSettingsSetup()
        import settings
        if "distmod" in dir( settings ):
            n = n + "-" + str( settings.distmod )
    except:
        pass

        
    dn = GetOption( "distmod" )
    if dn and len(dn) > 0:
        n = n + "-" + dn

    return n

def getCodeVersion():
    fullSource = open( "util/version.cpp" , "r" ).read()
    allMatches = re.findall( r"versionString.. = \"(.*?)\"" , fullSource );
    if len(allMatches) != 1:
        print( "can't find version # in code" )
        return None
    return allMatches[0]

if getCodeVersion() == None:
    Exit(-1)

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


    return utils.getGitBranchString( "" , "-" ) + today.strftime( "%Y-%m-%d" )


if distBuild:
    if isDriverBuild():
        installDir = GetOption( "prefix" )
    else:
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
        print( "************* " + str( target[0] ) + " has GLIBC_2.4 dependencies!" )
        Exit(-3)

allBinaries = []

def installBinary( e , name ):
    if not installSetup.binaries:
        return

    global allBinaries

    if windows:
        e.Alias( name , name + ".exe" )
        name += ".exe"

    inst = e.Install( installDir + "/bin" , name )

    fullInstallName = installDir + "/bin/" + name

    allBinaries += [ name ]
    if (solaris or linux) and (not has_option("nostrip")):
        e.AddPostAction( inst, e.Action( 'strip ' + fullInstallName ) )

    if not has_option( "no-glibc-check" ) and linux and len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) == "s3dist":
        e.AddPostAction( inst , checkGlibc )

    if nix:
        e.AddPostAction( inst , e.Action( 'chmod 755 ' + fullInstallName ) )

for x in normalTools:
    installBinary( env , "mongo" + x )
installBinary( env , "bsondump" )
#installBinary( env , "mongoperf" )

if mongosniff_built:
    installBinary(env, "mongosniff")

installBinary( env , "mongod" )
installBinary( env , "mongos" )

if not noshell:
    installBinary( env , "mongo" )

env.Alias( "all" , allBinaries )
env.Alias( "core" , [ add_exe( "mongo" ) , add_exe( "mongod" ) , add_exe( "mongos" ) ] )

#headers
if installSetup.headers:
    for id in [ "", "util/", "util/net/", "util/mongoutils/", "util/concurrency/", "db/" , "db/stats/" , "db/repl/" , "db/ops/" , "client/" , "bson/", "bson/util/" , "s/" , "scripting/" ]:
        env.Install( installDir + "/" + installSetup.headerRoot + "/mongo/" + id , Glob( id + "*.h" ) )
        env.Install( installDir + "/" + installSetup.headerRoot + "/mongo/" + id , Glob( id + "*.hpp" ) )

if installSetup.clientSrc:
    for x in allClientFiles:
        x = str(x)
        env.Install( installDir + "/mongo/" + x.rpartition( "/" )[0] , x )

#lib
if installSetup.libraries:
    env.Install( installDir + "/" + nixLibPrefix, clientLibName )
    if has_option( "sharedclient" ): 
        env.Install( installDir + "/" + nixLibPrefix, sharedClientLibName )


#textfiles
if installSetup.bannerDir:
    for x in os.listdir( installSetup.bannerDir ):
        full = installSetup.bannerDir + "/" + x
        if os.path.isdir( full ):
            continue
        if x.find( "~" ) >= 0:
            continue
        env.Install( installDir , full )

if installSetup.clientTestsDir:
    for x in os.listdir( installSetup.clientTestsDir ):
        full = installSetup.clientTestsDir + "/" + x
        if os.path.isdir( full ):
            continue
        if x.find( "~" ) >= 0:
            continue
        env.Install( installDir + '/' + installSetup.clientTestsDir , full )

#final alias
env.Alias( "install" , installDir )

# aliases
env.Alias( "mongoclient" , has_option( "sharedclient" ) and sharedClientLibName or clientLibName )


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
            remotePrefix = utils.getGitBranchString( "-" ) + "-latest"
        else:
            remotePrefix = "-" + distName

    findSettingsSetup()

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
        
    if isDriverBuild():
        name = "cxx-driver/" + name
    elif platformDir:
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

if installDir[-1] != "/":
    if windows:
        distFile = installDir + ".zip"
        env.Zip( distFile , installDir )
    else:
        distFile = installDir + ".tgz"
        env.Tar( distFile , installDir )

    env.Alias( "dist" , distFile )
    env.Alias( "s3dist" , [ "install"  , distFile ] , [ s3dist ] )
    env.AlwaysBuild( "s3dist" )


# client dist
def build_and_test_client(env, target, source):
    from subprocess import call

    if GetOption("extrapath") is not None:
        scons_command = ["scons", "--extrapath=" + GetOption("extrapath")]
    else:
        scons_command = ["scons"]

    call(scons_command + ["libmongoclient.a", "clientTests"], cwd=installDir)

    return bool(call(["python", "buildscripts/smoke.py",
                      "--test-path", installDir, "client"]))
env.Alias("clientBuild", [mongod, installDir], [build_and_test_client])
env.AlwaysBuild("clientBuild")

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

# --- an uninstall target ---
if len(COMMAND_LINE_TARGETS) > 0 and 'uninstall' in COMMAND_LINE_TARGETS:
    SetOption("clean", 1)
    # By inspection, changing COMMAND_LINE_TARGETS here doesn't do
    # what we want, but changing BUILD_TARGETS does.
    BUILD_TARGETS.remove("uninstall")
    BUILD_TARGETS.append("install")
