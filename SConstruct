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

EnsureSConsVersion( 1, 1, 0 )

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

import libdeps


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

def getThirdPartyShortNames():
    lst = []
    for x in os.listdir( "src/third_party" ):
        if not x.endswith( ".py" ) or x.find( "#" ) >= 0:
            continue

        lst.append( _rpartition( x, "." )[0] )
    return lst


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

    if name not in options_topass:
        # if someone already set this, don't overwrite
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

add_option("smokedbprefix", "prefix to dbpath et al. for smoke tests", 1 , False )
add_option("smokeauth", "run smoke tests with --auth", 0 , False )

for shortName in getThirdPartyShortNames():
    add_option( "use-system-" + shortName , "use system version of library " + shortName , 0 , True )

add_option( "use-system-pcre", "use system version of pcre library", 0, True )

add_option( "use-system-all" , "use all system libraries", 0 , True )

add_option( "use-cpu-profiler",
            "Link against the google-perftools profiler library",
            0, True )

# don't run configure if user calls --help
if GetOption('help'):
    Return()

# --- environment setup ---

variantDir = get_variant_dir()

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

env = Environment( BUILD_DIR=variantDir,
                   MSVS_ARCH=msarch ,
                   tools=["default", "gch", "jsheader", "mergelib" ],
                   PYSYSPLATFORM=os.sys.platform,

                   PCRE_VERSION='7.4',
                   )


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

if env['PYSYSPLATFORM'] == 'linux2':
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

env.Append( CPPDEFINES=[ "_SCONS" , "MONGO_EXPOSE_MACROS" ],
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

distBuild = len( COMMAND_LINE_TARGETS ) == 1 and ( str( COMMAND_LINE_TARGETS[0] ) == "s3dist" or str( COMMAND_LINE_TARGETS[0] ) == "dist" )

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
    clientSrc = False
    headers = False
    bannerFiles = tuple()
    headerRoot = "include"

    def __init__(self):
        self.default()
    
    def default(self):
        self.binaries = True
        self.libraries = False
        self.clientSrc = False
        self.headers = False
        self.bannerFiles = tuple()
        self.headerRoot = "include"
        self.clientTestsDir = None

    def justClient(self):
        self.binaries = False
        self.libraries = False
        self.clientSrc = True
        self.headers = True
        self.bannerFiles = [ "#distsrc/client/LICENSE.txt",
                             "#distsrc/client/SConstruct" ]
        self.headerRoot = "mongo/"
        self.clientTestsDir = "#src/mongo/client/examples/"

installSetup = InstallSetup()
if distBuild:
    installSetup.bannerFiles = [ "#distsrc/GNU-AGPL-3.0",
                                 "#distsrc/README",
                                 "#distsrc/THIRD-PARTY-NOTICES", ]

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
        installDir = '#' + installDir
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
        env.Append( EXTRACPPPATH=["/usr/64/include"] )
        env.Append( EXTRALIBPATH=["/usr/64/lib"] )
        if installDir == DEFAULT_INSTALL_DIR and not distBuild:
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

elif os.sys.platform.startswith( "freebsd" ):
    nix = True
    freebsd = True
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__freebsd__" ] )

elif os.sys.platform.startswith( "openbsd" ):
    nix = True
    openbsd = True
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
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

    env.Append( EXTRACPPPATH=[ boostDir , winSDKHome + "/Include" ] )

    # consider adding /MP build with multiple processes option.

    # /EHsc exception handling style for visual studio
    # /W3 warning level
    # /WX abort build on compiler warnings
    env.Append( CPPFLAGS=" /EHsc /W3 " ) #  /WX " )

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
        # /DEBUG will tell the linker to create a .pdb file
        # which WinDbg and Visual Studio will use to resolve
        # symbols if you want to debug a release-mode image
        env.Append( LINKFLAGS=" /DEBUG " )
    else:
        # /Od disable optimization
        # /Z7 debug info goes into each individual .obj file -- no .pdb created 
        # /TP it's a c++ file
        # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized
        env.Append( CPPFLAGS=" /RTC1 /MDd /Z7 /TP /errorReport:none " )

        if debugBuild:
            env.Append( LINKFLAGS=" /debug " )
            env.Append( CPPFLAGS=" /Od " )
            
        if debugLogging:
            env.Append( CPPDEFINES=[ "_DEBUG" ] )

    if force64 and os.path.exists( boostDir + "/lib/vs2010_64" ):
        env.Append( EXTRALIBPATH=[ boostDir + "/lib/vs2010_64" ] )
    elif not force64 and os.path.exists( boostDir + "/lib/vs2010_32" ):
        env.Append( EXTRALIBPATH=[ boostDir + "/lib/vs2010_32" ] )
    else:
        env.Append( EXTRALIBPATH=[ boostDir + "/Lib" ] )

    if force64:
        env.Append( EXTRALIBPATH=[ winSDKHome + "/Lib/x64" ] )
    else:
        env.Append( EXTRALIBPATH=[ winSDKHome + "/Lib" ] )

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

    # v8 calls timeGetTime()
    if usev8:
        winLibString += " winmm.lib "

    env.Append( LIBS=Split(winLibString) )

    # dm these should automatically be defined by the compiler. commenting out to see if works. jun2010
    #if force64:
    #    env.Append( CPPDEFINES=["_AMD64_=1"] )
    #else:
    #    env.Append( CPPDEFINES=["_X86_=1"] )

    env.Append( EXTRACPPPATH=["#/../winpcap/Include"] )
    env.Append( EXTRALIBPATH=["#/../winpcap/Lib"] )

else:
    print( "No special config for [" + os.sys.platform + "] which probably means it won't work" )

env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1
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
    env.Append( CXXFLAGS=" -Wnon-virtual-dtor -Woverloaded-virtual" )
    env.Append( LINKFLAGS=" -fPIC -pthread -rdynamic" )
    env.Append( LIBS=[] )

    #make scons colorgcc friendly
    env['ENV']['HOME'] = os.environ['HOME']
    try:
        env['ENV']['TERM'] = os.environ['TERM']
    except KeyError:
        pass

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
commonFiles = []
serverOnlyFiles = []
scriptingFiles = []
for shortName in getThirdPartyShortNames():
    path = "src/third_party/%s.py" % shortName
    myModule = imp.load_module( "src/third_party_%s" % shortName , open( path , "r" ) , path , ( ".py" , "r" , imp.PY_SOURCE ) )
    fileLists = { "commonFiles" : commonFiles , "serverOnlyFiles" : serverOnlyFiles , "scriptingFiles" : scriptingFiles, "moduleFiles" : moduleFiles }

    options_topass["windows"] = windows
    options_topass["nix"] = nix

    if has_option( "use-system-" + shortName ) or has_option( "use-system-all" ):
        print( "using system version of: " + shortName )
        myModule.configureSystem( env , fileLists , options_topass )
    else:
        myModule.configure( env , fileLists , options_topass )

if not has_option("use-system-all") and not has_option("use-system-pcre"):
    env.Append(CPPPATH=[ '$BUILD_DIR/third_party/pcre-${PCRE_VERSION}' ])

env.Append( CPPPATH=['$EXTRACPPPATH'],
            LIBPATH=['$EXTRALIBPATH'] )

env['MONGO_COMMON_FILES'] = commonFiles
env['MONGO_SERVER_ONLY_FILES' ] = serverOnlyFiles
env['MONGO_SCRIPTING_FILES'] = scriptingFiles
env['MONGO_MODULE_FILES'] = moduleFiles

# --- check system ---

def getSysInfo():
    if windows:
        return "windows " + str( sys.getwindowsversion() )
    else:
        return " ".join( os.uname() )

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
            allPlaces += myenv.subst( myenv["LIBPATH"] )
            if not force64:
                allPlaces += [ "/usr/lib" , "/usr/local/lib" ]

            for p in poss:
                for loc in allPlaces:
                    fullPath = loc + "/lib" + p + ".a"
                    if os.path.exists( fullPath ):
                        myenv.Append( _LIBFLAGS='${SLIBS}',
                                      SLIBS=" " + fullPath + " " )
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

testEnv = env.Clone()
testEnv.Append( CPPPATH=["../"] )

shellEnv = None
if noshell:
    print( "not building shell" )
elif not onlyServer:
    shellEnv = env.Clone();

    if release and ( ( darwin and force64 ) or linux64 ):
        shellEnv["LINKFLAGS"] = env["LINKFLAGS_CLEAN"]
        shellEnv["LIBS"] = env["LIBS_CLEAN"]
        shellEnv["SLIBS"] = ""

    if windows:
        shellEnv.Append( LIBS=["winmm.lib"] )

    shellEnv = doConfigure( shellEnv , shell=True )

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
        installDir = "#mongodb-" + getSystemInstallName() + "-"
        installDir += getDistName( installDir )
        print "going to make dist: " + installDir[1:]

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
        (root,dot,suffix) = _rpartition( localName, "." )
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
        distFile = env.Zip( installDir + ".zip", installDir )[0]
    else:
        distFile = env.Tar( installDir + '.tgz', installDir )[0]

    env.Alias( "dist" , distFile )
    env.Alias( "s3dist" , [ distFile ] , [ s3dist ] )
    env.AlwaysBuild( "s3dist" )

# --- an uninstall target ---
if len(COMMAND_LINE_TARGETS) > 0 and 'uninstall' in COMMAND_LINE_TARGETS:
    SetOption("clean", 1)
    # By inspection, changing COMMAND_LINE_TARGETS here doesn't do
    # what we want, but changing BUILD_TARGETS does.
    BUILD_TARGETS.remove("uninstall")
    BUILD_TARGETS.append("install")

# The following symbols are exported for use in subordinate SConscript files.
# Ideally, the SConscript files would be purely declarative.  They would only
# import build environment objects, and would contain few or no conditional
# statements or branches.
#
# Currently, however, the SConscript files do need some predicates for
# conditional decision making that hasn't been moved up to this SConstruct file,
# and they are exported here, as well.
Export("env")
Export("shellEnv")
Export("testEnv")
Export("has_option")
Export("installSetup getSysInfo")
Export("usesm usev8")
Export("darwin windows solaris linux nix")

env.SConscript( 'src/SConscript', variant_dir=variantDir, duplicate=False )
env.SConscript( 'SConscript.smoke' )

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
