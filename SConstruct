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

# This file, SConstruct, configures the build environment, and then delegates to
# several, subordinate SConscript files, which describe specific build rules.

import buildscripts
import buildscripts.bb
import datetime
import imp
import os
import re
import shutil
import stat
import sys
import types
import urllib
import urllib2
from buildscripts import utils

import libdeps

EnsureSConsVersion( 1, 1, 0 )
if "uname" in dir(os):
    scons_data_dir = ".scons/%s/%s" % ( os.uname()[0] , os.getenv( "HOST" , "nohost" ) )
else:
    scons_data_dir = ".scons/%s/" % os.getenv( "HOST" , "nohost" )
SConsignFile( scons_data_dir + "/sconsign" )

DEFAULT_INSTALL_DIR = "/usr/local"

def _rpartition(string, sep):
    """A replacement for str.rpartition which is missing in Python < 2.5
    """
    idx = string.rfind(sep)
    if idx == -1:
        return '', '', string
    return string[:idx], sep, string[idx + 1:]



buildscripts.bb.checkOk()

def findSettingsSetup():
    sys.path.append( "." )
    sys.path.append( ".." )
    sys.path.append( "../../" )


# --- options ----

options = {}

options_topass = {}

def add_option( name, help, nargs, contributesToVariantDir,
                dest=None, default = None, type="string", choices=None ):

    if dest is None:
        dest = name

    AddOption( "--" + name , 
               dest=dest,
               type=type,
               nargs=nargs,
               action="store",
               choices=choices,
               default=default,
               help=help )

    options[name] = { "help" : help ,
                      "nargs" : nargs , 
                      "contributesToVariantDir" : contributesToVariantDir ,
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

    if name not in options_topass:
        # if someone already set this, don't overwrite
        options_topass[name] = x

    return x

def use_system_version_of_library(name):
    return has_option('use-system-all') or has_option('use-system-' + name)

def get_variant_dir():
    
    a = []
    
    for name in options:
        o = options[name]
        if not has_option( o["dest"] ):
            continue
        if not o["contributesToVariantDir"]:
            continue
        
        if o["nargs"] == 0:
            a.append( name )
        else:
            x = get_option( name )
            x = re.sub( "[,\\\\/]" , "_" , x )
            a.append( name + "_" + x )
            
    s = "#build/${PYSYSPLATFORM}/"

    if len(a) > 0:
        a.sort()
        s += "/".join( a ) + "/"
    else:
        s += "normal/"
    return s
        
# build output
add_option( "mute" , "do not display commandlines for compiling and linking, to reduce screen noise", 0, False )

# installation/packaging
add_option( "prefix" , "installation prefix" , 1 , False, default=DEFAULT_INSTALL_DIR )
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

# dev options
add_option( "d", "debug build no optimization, etc..." , 0 , True , "debugBuild" )
add_option( "dd", "debug build no optimization, additional debug logging, etc..." , 0 , True , "debugBuildAndLogging" )
add_option( "durableDefaultOn" , "have durable default to on" , 0 , True )
add_option( "durableDefaultOff" , "have durable default to off" , 0 , True )

add_option( "pch" , "use precompiled headers to speed up the build (experimental)" , 0 , True , "usePCH" )
add_option( "distcc" , "use distcc for distributing builds" , 0 , False )
add_option( "clang" , "use clang++ rather than g++ (experimental)" , 0 , True )

# debugging/profiling help

add_option( "tcmalloc" , "link against tcmalloc" , 0 , False )
add_option( "gdbserver" , "build in gdb server support" , 0 , True )
add_option( "heapcheck", "link to heap-checking malloc-lib and look for memory leaks during tests" , 0 , False )
add_option( "gcov" , "compile with flags for gcov" , 0 , True )

add_option("smokedbprefix", "prefix to dbpath et al. for smoke tests", 1 , False )
add_option("smokeauth", "run smoke tests with --auth", 0 , False )

add_option( "use-system-pcre", "use system version of pcre library", 0, True )

add_option( "use-system-boost", "use system version of boost libraries", 0, True )

add_option( "use-system-snappy", "use system version of snappy library", 0, True )

add_option( "use-system-sm", "use system version of spidermonkey library", 0, True )

add_option( "use-system-all" , "use all system libraries", 0 , True )

add_option( "use-cpu-profiler",
            "Link against the google-perftools profiler library",
            0, True )

add_option("mongod-concurrency-level", "Concurrency level, \"global\" or \"db\"", 1, True,
           type="choice", choices=["global", "db"])

add_option('client-dist-basename', "Name of the client source archive.", 1, False,
           default='mongo-cxx-driver')

# don't run configure if user calls --help
if GetOption('help'):
    Return()

# --- environment setup ---

variantDir = get_variant_dir()

def printLocalInfo():
    import sys, SCons
    print( "scons version: " + SCons.__version__ )
    print( "python version: " + " ".join( [ `i` for i in sys.version_info ] ) )

printLocalInfo()

boostLibs = [ "thread" , "filesystem" , "program_options", "system" ]

onlyServer = len( COMMAND_LINE_TARGETS ) == 0 or ( len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) in [ "mongod" , "mongos" , "test" ] )
nix = False
linux = False
linux64  = False
darwin = False
windows = False
freebsd = False
openbsd = False
solaris = False
bigendian = False # For snappy
force32 = has_option( "force32" ) 
force64 = has_option( "force64" )
if not force64 and not force32 and os.getcwd().endswith( "mongo-64" ):
    force64 = True
    print( "*** assuming you want a 64-bit build b/c of directory *** " )
msarch = None
if force32:
    msarch = "x86"
elif force64:
    msarch = "amd64"

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

env = Environment( BUILD_DIR=variantDir,
                   CLIENT_ARCHIVE='${CLIENT_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
                   CLIENT_DIST_BASENAME=get_option('client-dist-basename'),
                   CLIENT_LICENSE='#distsrc/client/LICENSE.txt',
                   CLIENT_SCONSTRUCT='#distsrc/client/SConstruct',
                   DIST_ARCHIVE_SUFFIX='.tgz',
                   EXTRAPATH=get_option("extrapath"),
                   MSVS_ARCH=msarch ,
                   PYTHON=utils.find_python(),
                   SERVER_ARCHIVE='${SERVER_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
                   TARGET_ARCH=msarch ,
                   tools=["default", "gch", "jsheader", "mergelib"],
                   PYSYSPLATFORM=os.sys.platform,

                   PCRE_VERSION='8.30',
                   CONFIGUREDIR = scons_data_dir + '/sconf_temp',
                   CONFIGURELOG = scons_data_dir + '/config.log'
                   )

if has_option('mute'):
    env.Append( CCCOMSTR = "Compiling $TARGET" )
    env.Append( CXXCOMSTR = env["CCCOMSTR"] )
    env.Append( LINKCOMSTR = "Linking $TARGET" )
    env.Append( ARCOMSTR = "Generating library $TARGET" )

if has_option('mongod-concurrency-level'):
    env.Append(CPPDEFINES=['MONGOD_CONCURRENCY_LEVEL=MONGOD_CONCURRENCY_LEVEL_%s' % get_option('mongod-concurrency-level').upper()])

libdeps.setup_environment( env )

if env['PYSYSPLATFORM'] == 'linux3':
    env['PYSYSPLATFORM'] = 'linux2'

if os.sys.platform == 'win32':
    env['OS_FAMILY'] = 'win'
else:
    env['OS_FAMILY'] = 'posix'

if has_option( "cxx" ):
    env["CC"] = get_option( "cxx" )
    env["CXX"] = get_option( "cxx" )
elif has_option("clang"):
    env["CC"] = 'clang'
    env["CXX"] = 'clang++'

if has_option( "cc" ):
    env["CC"] = get_option( "cc" )

if env['PYSYSPLATFORM'] == 'linux2' or env['PYSYSPLATFORM'].startswith( 'freebsd' ):
    env['LINK_LIBGROUP_START'] = '-Wl,--start-group'
    env['LINK_LIBGROUP_END'] = '-Wl,--end-group'
    env['RELOBJ_LIBDEPS_START'] = '--whole-archive'
    env['RELOBJ_LIBDEPS_END'] = '--no-whole-archive'
    env['RELOBJ_LIBDEPS_ITEM'] = ''
elif env['PYSYSPLATFORM'] == 'darwin':
    env['RELOBJFLAGS'] = [ '-arch', '$PROCESSOR_ARCHITECTURE' ]
    env['LINK_LIBGROUP_START'] = ''
    env['LINK_LIBGROUP_END'] = ''
    env['RELOBJ_LIBDEPS_START'] = '-all_load'
    env['RELOBJ_LIBDEPS_END'] = ''
    env['RELOBJ_LIBDEPS_ITEM'] = ''
elif env['PYSYSPLATFORM'].startswith('sunos'):
    if force64:
        env['RELOBJFLAGS'] = ['-64']
    env['LINK_LIBGROUP_START'] = '-z rescan'
    env['LINK_LIBGROUP_END'] = ''
    env['RELOBJ_LIBDEPS_START'] = '-z allextract'
    env['RELOBJ_LIBDEPS_END'] = '-z defaultextract'
    env['RELOBJ_LIBDEPS_ITEM'] = ''

env["LIBPATH"] = []

if has_option( "libpath" ):
    env["LIBPATH"] = [get_option( "libpath" )]

if has_option( "cpppath" ):
    env["CPPPATH"] = [get_option( "cpppath" )]

env.Prepend( CPPDEFINES=[ "_SCONS" , 
                          "MONGO_EXPOSE_MACROS" ,
                          "SUPPORT_UTF8" ],  # for pcre


             CPPPATH=[ '$BUILD_DIR', "$BUILD_DIR/mongo" ] )

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

extraLibPlaces = []

env['EXTRACPPPATH'] = []
env['EXTRALIBPATH'] = []

def addExtraLibs( s ):
    for x in s.split(","):
        env.Append( EXTRACPPPATH=[ x + "/include" ] )
        env.Append( EXTRALIBPATH=[ x + "/lib" ] )
        env.Append( EXTRALIBPATH=[ x + "/lib64" ] )
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
    libraries = False
    headers = False

    def __init__(self):
        self.default()

    def default(self):
        self.binaries = True
        self.libraries = False
        self.headers = False

installSetup = InstallSetup()

if has_option( "full" ):
    installSetup.headers = True
    installSetup.libraries = True

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

env['PROCESSOR_ARCHITECTURE'] = processor

installDir = DEFAULT_INSTALL_DIR
nixLibPrefix = "lib"

dontReplacePackage = False
isBuildingLatest = False

if has_option( "prefix" ):
    installDir = GetOption( "prefix" )

def findVersion( root , choices ):
    if not isinstance(root, list):
        root = [root]
    for r in root:
        for c in choices:
            if ( os.path.exists( r + c ) ):
                return r + c
    raise RuntimeError("can't find a version of [" + repr(root) + "] choices: " + repr(choices))

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
       env.Append( EXTRACPPPATH=["/usr/64/include"] )
       env.Append( EXTRALIBPATH=["/usr/64/lib"] )
       if installDir == DEFAULT_INSTALL_DIR:
           installDir = "/usr/64/"
    else:
       env.Append( EXTRACPPPATH=filterExists(["/sw/include" , "/opt/local/include"]) )
       env.Append( EXTRALIBPATH=filterExists(["/sw/lib/", "/opt/local/lib"]) )

elif os.sys.platform.startswith("linux"):
    linux = True
    platform = "linux"

    env.Append( LIBS=['m'] )

    if os.uname()[4] == "x86_64" and not force32:
        linux64 = True
        nixLibPrefix = "lib64"
        env.Append( EXTRALIBPATH=["/usr/lib64" , "/lib64" ] )
        env.Append( LIBS=["pthread"] )

        force64 = False

    if force32:
        env.Append( EXTRALIBPATH=["/usr/lib32"] )

    nix = True

    if static:
        env.Append( LINKFLAGS=" -static " )

elif "sunos5" == os.sys.platform:
     nix = True
     solaris = True
     env.Append( CPPDEFINES=[ "__sunos__" ] )
     env.Append( LIBS=["socket","resolv"] )
     # Need v9 for atomics when sparc
     if processor.startswith( "sun4" ):
        env.Append( CCFLAGS=[ "-mcpu=v9", "-m64" ] )
        env.Append( LINKFLAGS=[ "-m64" ] )

elif os.sys.platform.startswith( "freebsd" ):
    nix = True
    freebsd = True
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__freebsd__" ] )
    env.Append( LIBS=['m'] )

elif os.sys.platform.startswith( "openbsd" ):
    nix = True
    openbsd = True
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__openbsd__" ] )

elif "win32" == os.sys.platform:
    windows = True

    env['DIST_ARCHIVE_SUFFIX'] = '.zip'

    if has_option( "win2008plus" ):
        env.Append( CPPDEFINES=[ "MONGO_USE_SRW_ON_WINDOWS" ] )

    for pathdir in env['ENV']['PATH'].split(os.pathsep):
	if os.path.exists(os.path.join(pathdir, 'cl.exe')):
            print( "found visual studio at " + pathdir )
	    break
    else:
	#use current environment
	env['ENV'] = dict(os.environ)

    env.Append( CPPDEFINES=[ "_UNICODE" ] )
    env.Append( CPPDEFINES=[ "UNICODE" ] )

    winSDKHome = findVersion( [ "C:/Program Files/Microsoft SDKs/Windows/", "C:/Program Files (x86)/Microsoft SDKs/Windows/" ] ,
                              [ "v7.1", "v7.0A", "v7.0", "v6.1", "v6.0a", "v6.0" ] )
    print( "Windows SDK Root '" + winSDKHome + "'" )

    env.Append( EXTRACPPPATH=[ winSDKHome + "/Include" ] )

    # /EHsc exception handling style for visual studio
    # /W3 warning level
    # /WX abort build on compiler warnings
    env.Append(CCFLAGS=["/EHsc","/W3"])

    # some warnings we don't like:
    # c4355
    # 'this' : used in base member initializer list
    #    The this pointer is valid only within nonstatic member functions. It cannot be used in the initializer list for a base class.
    # c4800
    # 'type' : forcing value to bool 'true' or 'false' (performance warning)
    #    This warning is generated when a value that is not bool is assigned or coerced into type bool. 
    # c4267
    # 'var' : conversion from 'size_t' to 'type', possible loss of data
    # When compiling with /Wp64, or when compiling on a 64-bit operating system, type is 32 bits but size_t is 64 bits when compiling for 64-bit targets. To fix this warning, use size_t instead of a type.
    # c4244
    # 'conversion' conversion from 'type1' to 'type2', possible loss of data
    #  An integer type is converted to a smaller integer type.
    env.Append( CCFLAGS=["/wd4355", "/wd4800", "/wd4267", "/wd4244"] )
    
    # PSAPI_VERSION relates to process api dll Psapi.dll.
    env.Append( CPPDEFINES=["_CONSOLE","_CRT_SECURE_NO_WARNINGS","PSAPI_VERSION=1" ] )

    # this would be for pre-compiled headers, could play with it later  
    #env.Append( CCFLAGS=['/Yu"pch.h"'] )

    # docs say don't use /FD from command line (minimal rebuild)
    # /Gy function level linking (implicit when using /Z7)
    # /Z7 debug info goes into each individual .obj file -- no .pdb created 
    env.Append( CCFLAGS= ["/Z7", "/errorReport:none"] )
    if release:
        # /MT: Causes your application to use the multithread, static version of the run-time library (LIBCMT.lib)
        # /O2: optimize for speed (as opposed to size)
        env.Append( CCFLAGS= ["/O2", "/MT"] )

        # TODO: this has caused some linking problems :
        # /GL whole program optimization
        # /LTCG link time code generation
        env.Append( CCFLAGS= ["/GL"] )
        env.Append( LINKFLAGS=" /LTCG " )
        env.Append( ARFLAGS=" /LTCG " ) # for the Library Manager
        # /DEBUG will tell the linker to create a .pdb file
        # which WinDbg and Visual Studio will use to resolve
        # symbols if you want to debug a release-mode image.
        # Note that this means we can't do parallel links in the build.
        env.Append( LINKFLAGS=" /DEBUG " )
    else:
        # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized
        #        (implies /Od: no optimizations)
        # /MTd: Defines _DEBUG, _MT, and causes your application to use the
        #       debug multithread version of the run-time library (LIBCMTD.lib)
        env.Append( CCFLAGS=["/RTC1", "/Od", "/MTd"] )
        if debugBuild:
            # If you build without --d, no debug PDB will be generated, and 
            # linking will be faster. However, you won't be able to debug your code with the debugger.
            env.Append( LINKFLAGS=" /debug " )
        #if debugLogging:
            # This is already implicit from /MDd...
            #env.Append( CPPDEFINES=[ "_DEBUG" ] )
            # This means --dd is always on unless you say --release

    if force64:
        env.Append( EXTRALIBPATH=[ winSDKHome + "/Lib/x64" ] )
    else:
        env.Append( EXTRALIBPATH=[ winSDKHome + "/Lib" ] )

    if release:
        env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  " )
    else:
        env.Append( LINKFLAGS=" /NODEFAULTLIB:MSVCPRT  /NODEFAULTLIB:MSVCRT  " )

    winLibString = "ws2_32.lib kernel32.lib advapi32.lib Psapi.lib DbgHelp.lib"

    if force64:

        winLibString += ""

    else:
        winLibString += " user32.lib gdi32.lib winspool.lib comdlg32.lib  shell32.lib ole32.lib oleaut32.lib "
        winLibString += " odbc32.lib odbccp32.lib uuid.lib "

    # v8 calls timeGetTime()
    if usev8:
        winLibString += " winmm.lib "

    env.Append( LIBS=Split(winLibString) )

    env.Append( EXTRACPPPATH=["#/../winpcap/Include"] )
    env.Append( EXTRALIBPATH=["#/../winpcap/Lib"] )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )

env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1
if nix:

    if has_option( "distcc" ):
        env["CXX"] = "distcc " + env["CXX"]

    # -Winvalid-pch Warn if a precompiled header (see Precompiled Headers) is found in the search path but can't be used.
    env.Append( CCFLAGS=["-fPIC",
                         "-fno-strict-aliasing",
                         "-Wstrict-aliasing",
                         "-ggdb",
                         "-pthread",
                         "-Wall",
                         "-Wsign-compare",
                         "-Wno-unknown-pragmas",
                         "-Wcast-align",
                         "-Winvalid-pch"] )
    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    if linux and ( processor == "i386" or processor == "x86_64" ):
        env.Append( CCFLAGS=["-Werror", "-pipe"] )
        if not has_option('clang'):
            env.Append( CCFLAGS=["-fno-builtin-memcmp"] ) # glibc's memcmp is faster than gcc's

    env.Append( CPPDEFINES=["_FILE_OFFSET_BITS=64"] )
    env.Append( CXXFLAGS=["-Wnon-virtual-dtor", "-Woverloaded-virtual"] )
    env.Append( LINKFLAGS=["-fPIC", "-pthread",  "-rdynamic"] )
    env.Append( LIBS=[] )

    #make scons colorgcc friendly
    env['ENV']['HOME'] = os.environ['HOME']
    try:
        env['ENV']['TERM'] = os.environ['TERM']
    except KeyError:
        pass

    if linux and has_option( "sharedclient" ):
        env.Append( LINKFLAGS=" -Wl,--as-needed -Wl,-zdefs " )

    if linux and has_option( "gcov" ):
        env.Append( CXXFLAGS=" -fprofile-arcs -ftest-coverage " )
        env.Append( LINKFLAGS=" -fprofile-arcs -ftest-coverage " )

    if debugBuild:        
        env.Append( CCFLAGS=["-O0" ] )
        if not solaris:
            env.Append( CCFLAGS=["-fstack-protector" ] )
        env['ENV']['GLIBCXX_FORCE_NEW'] = 1; # play nice with valgrind
    else:
        env.Append( CCFLAGS=["-O3"] )

    if debugLogging:
        env.Append( CPPDEFINES=["_DEBUG"] );

    if force64:
        env.Append( CCFLAGS="-m64" )
        env.Append( LINKFLAGS="-m64" )

    if force32:
        env.Append( CCFLAGS="-m32" )
        env.Append( LINKFLAGS="-m32" )

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
        env['Gch'] = env.Gch( "$BUILD_DIR/mongo/pch.h$GCHSUFFIX",
                              "src/mongo/pch.h" )[0]
        env['GchSh'] = env[ 'Gch' ]
    elif os.path.exists( env.File("$BUILD_DIR/mongo/pch.h$GCHSUFFIX").abspath ):
        print( "removing precompiled headers" )
        os.unlink( env.File("$BUILD_DIR/mongo/pch.h.$GCHSUFFIX").abspath ) # gcc uses the file if it exists

if usev8:
    env.Prepend( EXTRACPPPATH=["#/../v8/include/"] )
    env.Prepend( EXTRALIBPATH=["#/../v8/"] )

if usesm:
    env.Append( CPPDEFINES=["JS_C_STRINGS_ARE_UTF8"] )

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

if not use_system_version_of_library("pcre"):
    env.Prepend(CPPPATH=[ '$BUILD_DIR/third_party/pcre-${PCRE_VERSION}' ])

if not use_system_version_of_library("boost"):
    env.Prepend(CPPPATH=['$BUILD_DIR/third_party/boost'],
                CPPDEFINES=['BOOST_ALL_NO_LIB'])

env.Append( CPPPATH=['$EXTRACPPPATH'],
            LIBPATH=['$EXTRALIBPATH'] )

# --- check system ---

def CheckStackProtector( context ):
    oldCFLAGS = context.env['CFLAGS']
    context.env['CFLAGS'] = context.env['CFLAGS'] + " -fstack-protector "
    context.Message( 'Checking if -fstack-protector works ...' )
    res = context.TryLink( """
          void __attribute__((noinline)) rolf( char* x ) {
             for ( int i = 0; i < 8; ++i ) {
                x[i] = 0;
             }             
          }
          int main( int argc, char** argv ) 
          { 
              rolf( argv[0] );
          }
""", ".cc" )
    context.env['CFLAGS'] = oldCFLAGS
    context.Result( res )
    return res


def CheckFetchAndAdd( context ):
    context.Message( 'Checking for __sync_fetch_and_add ...' )
    res = context.TryLink( """
          int main() 
          { 
            int x; 
            __sync_fetch_and_add(&x, 1); 
            __sync_add_and_fetch(&x, 1);
            return 0; 
          }
""", ".cc" )
    context.Result( res )
    return res

def CheckSwap32( context ):
    context.Message( 'Checking for inline __builtin_bswap32 ...' )
    res = context.TryCompile( """
           int foo( int x ) {
               return __builtin_bswap32( x );
           }
""", ".cc" )
    
    res = res and not 'bswap' in context.lastTarget.get_contents()
    
    context.Result( res )
    return res

def CheckSwap64( context ):
    context.Message( 'Checking for inline __builtin_bswap64 ...' )
    res = context.TryCompile( """
           long long foo( long long x ) {
               return __builtin_bswap64( x );
           }
""", ".cc" )
    
    res = res and not 'bswap' in context.lastTarget.get_contents()
    
    context.Result( res )
    return res

def CheckAlignment( context ):
    oldCFLAGS = context.env['CFLAGS']
    context.env['CFLAGS'] = context.env['CFLAGS'] + " -Wcast-align -Werror "
    context.Message( 'Checking if alignment is important ...' )
    res = context.TryLink( """
          int main(int argc, char** argv)
          {
              int* y = (int*)argv[0];
              return *y;
          }
""", ".c" )
    context.env['CFLAGS'] = oldCFLAGS
    res = not res
    context.Result( res )
    return res
   
def CheckBigEndian( context ):
    context.Message( 'Checking if system is big endian (for snappy) ...' )
    # Include path will be sprintf:ed in below
    prog = """
          #include "%s/src/third_party/boost/boost/detail/endian.hpp"
          #ifdef BOOST_LITTLE_ENDIAN
          #error "Little Endian"
          #endif
"""

    res = context.TryCompile( prog % ( Dir('#').abspath ), ".cc" ) 
    context.Result( res )
    return res

def doConfigure(myenv):
    conf = Configure(myenv, custom_tests = {
        'CheckFetchAndAdd' : CheckFetchAndAdd,
        'CheckAlignment' : CheckAlignment,
        'CheckSwap32' : CheckSwap32,
        'CheckSwap64' : CheckSwap64,
        'CheckBigEndian' : CheckBigEndian,
        'CheckStackProtector' : CheckStackProtector,
        })

    if 'CheckCXX' in dir( conf ):
        if  not conf.CheckCXX():
            print( "c++ compiler not installed!" )
            Exit(1)

    if use_system_version_of_library("boost"):
        if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
            print( "can't find boost headers" )
            Exit(1)

        for b in boostLibs:
            l = "boost_" + b
            if not conf.CheckLib([ l + boostCompiler + "-mt" + boostVersion,
                                   l + boostCompiler + boostVersion ], language='C++' ):
                Exit(1)

    if conf.CheckHeader('unistd.h'):
        myenv.Append(CPPDEFINES=['MONGO_HAVE_HEADER_UNISTD_H'])

    if solaris or conf.CheckDeclaration('clock_gettime', includes='#include <time.h>'):
        conf.CheckLib('rt')

    if (conf.CheckCXXHeader( "execinfo.h" ) and
        conf.CheckDeclaration('backtrace', includes='#include <execinfo.h>') and
        conf.CheckDeclaration('backtrace_symbols', includes='#include <execinfo.h>')):

        myenv.Append( CPPDEFINES=[ "MONGO_HAVE_EXECINFO_BACKTRACE" ] )

    myenv["_HAVEPCAP"] = conf.CheckLib( ["pcap", "wpcap"], autoadd=False )

    if solaris:
        conf.CheckLib( "nsl" )

    if usev8:
        if debugBuild:
            v8_lib_choices = ["v8_g", "v8"]
        else:
            v8_lib_choices = ["v8"]
        if not conf.CheckLib( v8_lib_choices ):
            Exit(1)

    # requires ports devel/libexecinfo to be installed
    if freebsd or openbsd:
        if not conf.CheckLib("execinfo"):
            Exit(1)

    # Look for __sync_add_and_fetch and __sync_fetch_and_add
    if conf.CheckFetchAndAdd():
        env.Append( CPPDEFINES = ["HAVE_SYNC_FETCH_AND_ADD"] )
    # Check if natural alignment is important
    if conf.CheckAlignment():
        env.Append( CPPDEFINES = ["ALIGNMENT_IMPORTANT"] )
    # Look for inline __builtin_bswap32
    if conf.CheckSwap32():
        env.Append( CPPDEFINES = ["HAVE_BSWAP32"] )
    if conf.CheckSwap64():
        env.Append( CPPDEFINES = ["HAVE_BSWAP64"] )
    # Check endianess for snappy
    bigendian = conf.CheckBigEndian()
    Export( "bigendian" )

    if not conf.CheckStackProtector():
        try:
            env['CCFLAGS'].remove( '-fstack-protector' )
        except ValueError:
            pass
        
       
    # 'tcmalloc' needs to be the last library linked. Please, add new libraries before this 
    # point.
    if has_option("tcmalloc") or has_option("heapcheck"):
        if not conf.CheckLib("tcmalloc"):
            Exit(1)

    if has_option("heapcheck"):
        if ( not debugBuild ) and ( not debugLogging ):
            print( "--heapcheck needs --d or --dd" )
            Exit( 1 )

        if not conf.CheckCXXHeader( "google/heap-checker.h" ):
            print( "--heapcheck neads header 'google/heap-checker.h'" )
            Exit( 1 )

        myenv.Append( CPPDEFINES=[ "HEAP_CHECKING" ] )
        myenv.Append( CCFLAGS=["-fno-omit-frame-pointer"] )

    return conf.Finish()

env = doConfigure( env )

testEnv = env.Clone()
testEnv.Append( CPPPATH=["../"] )

shellEnv = None
if noshell:
    print( "not building shell" )
elif not onlyServer:
    shellEnv = env.Clone();

    if release and ( ( darwin and force64 ) or linux64 ):
        shellEnv["SLIBS"] = []

    if windows:
        shellEnv.Append( LIBS=["winmm.lib"] )

def checkErrorCodes():
    import buildscripts.errorcodes as x
    if x.checkErrorCodes() == False:
        print( "next id to use:" + str( x.getNextCode() ) )
        Exit(-1)

checkErrorCodes()

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
    fullSource = open( "src/mongo/util/version.cpp" , "r" ).read()
    allMatches = re.findall( r"versionString.. = \"(.*?)\"" , fullSource );
    if len(allMatches) != 1:
        print( "can't find version # in code" )
        return None
    return allMatches[0]

mongoCodeVersion = getCodeVersion()
if mongoCodeVersion == None:
    Exit(-1)

if has_option('distname'):
    distName = GetOption( "distname" )
elif mongoCodeVersion[-1] not in ("+", "-"):
    dontReplacePackage = True
    distName = mongoCodeVersion
else:
    isBuildingLatest = True
    distName = utils.getGitBranchString("" , "-") + datetime.date.today().strftime("%Y-%m-%d")


env['SERVER_DIST_BASENAME'] = 'mongodb-%s-%s' % (getSystemInstallName(), distName)

distFile = "${SERVER_ARCHIVE}"

env['NIX_LIB_DIR'] = nixLibPrefix
env['INSTALL_DIR'] = installDir
if testEnv is not None:
    testEnv['INSTALL_DIR'] = installDir
if shellEnv is not None:
    shellEnv['INSTALL_DIR'] = installDir


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
    localName = str( localName )

    if remotePrefix is None:
        if isBuildingLatest:
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
        (root,dot,suffix) = _rpartition( localName, "." )
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
    s3push( str(source[0]) , "mongodb" )

def s3distclient(env, target, source):
    s3push(str(source[0]), "cxx-driver/mongodb", platformDir=False)

env.Alias( "dist" , '$SERVER_ARCHIVE' )
env.Alias( "distclient", "$CLIENT_ARCHIVE")
env.AlwaysBuild(env.Alias( "s3dist" , [ '$SERVER_ARCHIVE' ] , [ s3dist ] ))
env.AlwaysBuild(env.Alias( "s3distclient" , [ '$CLIENT_ARCHIVE' ] , [ s3distclient ] ))

# --- an uninstall target ---
if len(COMMAND_LINE_TARGETS) > 0 and 'uninstall' in COMMAND_LINE_TARGETS:
    SetOption("clean", 1)
    # By inspection, changing COMMAND_LINE_TARGETS here doesn't do
    # what we want, but changing BUILD_TARGETS does.
    BUILD_TARGETS.remove("uninstall")
    BUILD_TARGETS.append("install")

clientEnv = env.Clone()
clientEnv['CPPDEFINES'].remove('MONGO_EXPOSE_MACROS')

if not use_system_version_of_library("boost"):
    clientEnv.Append(LIBS=['boost_thread', 'boost_filesystem', 'boost_system'])
    clientEnv.Prepend(LIBPATH=['$BUILD_DIR/third_party/boost/'])

clientEnv.Prepend(LIBS=['mongoclient'], LIBPATH=['.'])

# The following symbols are exported for use in subordinate SConscript files.
# Ideally, the SConscript files would be purely declarative.  They would only
# import build environment objects, and would contain few or no conditional
# statements or branches.
#
# Currently, however, the SConscript files do need some predicates for
# conditional decision making that hasn't been moved up to this SConstruct file,
# and they are exported here, as well.
Export("env")
Export("clientEnv")
Export("shellEnv")
Export("testEnv")
Export("has_option use_system_version_of_library")
Export("installSetup")
Export("usesm usev8")
Export("darwin windows solaris linux nix freebsd")

env.SConscript( 'src/SConscript', variant_dir='$BUILD_DIR', duplicate=False )
env.SConscript( 'src/SConscript.client', variant_dir='$BUILD_DIR/client_build', duplicate=False )
env.SConscript( ['SConscript.buildinfo', 'SConscript.smoke'] )

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

env.Alias('all', ['core', 'tools', 'clientTests', 'test'])
