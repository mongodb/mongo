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
import copy
import datetime
import imp
import os
import re
import shutil
import stat
import sys
import textwrap
import types
import urllib
import urllib2
from buildscripts import utils
from buildscripts import moduleconfig

import libdeps

EnsureSConsVersion( 1, 1, 0 )
if "uname" in dir(os):
    scons_data_dir = ".scons/%s/%s" % ( os.uname()[0] , os.getenv( "HOST" , "nohost" ) )
else:
    scons_data_dir = ".scons/%s/" % os.getenv( "HOST" , "nohost" )
SConsignFile( scons_data_dir + "/sconsign" )

DEFAULT_INSTALL_DIR = "/usr/local"


def findSettingsSetup():
    sys.path.append( "." )
    sys.path.append( ".." )
    sys.path.append( "../../" )


# --- options ----

options = {}

options_topass = {}

def add_option( name, help, nargs, contributesToVariantDir,
                dest=None, default = None, type="string", choices=None, metavar=None ):

    if dest is None:
        dest = name

    if type == 'choice' and not metavar:
        metavar = '[' + '|'.join(choices) + ']'

    AddOption( "--" + name , 
               dest=dest,
               type=type,
               nargs=nargs,
               action="store",
               choices=choices,
               default=default,
               metavar=metavar,
               help=help )

    options[name] = { "help" : help ,
                      "nargs" : nargs ,
                      "contributesToVariantDir" : contributesToVariantDir ,
                      "dest" : dest,
                      "default": default }

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

    substitute = lambda x: re.sub( "[:,\\\\/]" , "_" , x )

    a = []

    for name in options:
        o = options[name]
        if not has_option( o["dest"] ):
            continue
        if not o["contributesToVariantDir"]:
            continue
        if get_option(o["dest"]) == o["default"]:
            continue

        if o["nargs"] == 0:
            a.append( name )
        else:
            x = substitute( get_option( name ) )
            a.append( name + "_" + x )

    s = "#build/${PYSYSPLATFORM}/"

    extras = []
    if has_option("extra-variant-dirs"):
        extras = [substitute(x) for x in get_option( 'extra-variant-dirs' ).split( ',' )]

    if has_option("add-branch-to-variant-dir"):
        extras += ["branch_" + substitute( utils.getGitBranch() )]
    a += extras

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
add_option( "extra-variant-dirs", "extra variant dir components, separated by commas", 1, False)
add_option( "add-branch-to-variant-dir", "add current git branch to the variant dir", 0, False )

add_option( "sharedclient", "build a libmongoclient.so/.dll" , 0 , False )
add_option( "full", "include client and headers when doing scons install", 0 , False )

# linking options
add_option( "release" , "release build" , 0 , True )
add_option( "static" , "fully static build" , 0 , False )
add_option( "static-libstdc++" , "statically link libstdc++" , 0 , False )
add_option( "lto", "enable link time optimizations (experimental, except with MSVC)" , 0 , True )
add_option( "dynamic-windows", "dynamically link on Windows", 0, True)

# base compile flags
add_option( "64" , "whether to force 64 bit" , 0 , True , "force64" )
add_option( "32" , "whether to force 32 bit" , 0 , True , "force32" )

add_option( "cxx", "compiler to use" , 1 , True )
add_option( "cc", "compiler to use for c" , 1 , True )
add_option( "cc-use-shell-environment", "use $CC from shell for C compiler" , 0 , False )
add_option( "cxx-use-shell-environment", "use $CXX from shell for C++ compiler" , 0 , False )
add_option( "ld", "linker to use" , 1 , True )
add_option( "c++11", "enable c++11 support (experimental)", 0, True )

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
add_option( "usev8" , "use v8 for javascript" , 0 , True )
add_option( "libc++", "use libc++ (experimental, requires clang)", 0, True )

# mongo feature options
add_option( "noshell", "don't build shell" , 0 , True )
add_option( "safeshell", "don't let shell scripts run programs (still, don't run untrusted scripts)" , 0 , True )
add_option( "win2008plus",
            "use newer operating system API features (deprecated, use win-version-min instead)" ,
            0 , False )

# dev options
add_option( "d", "debug build no optimization, etc..." , 0 , True , "debugBuild" )
add_option( "dd", "debug build no optimization, additional debug logging, etc..." , 0 , True , "debugBuildAndLogging" )

sanitizer_choices = ["address", "memory", "thread", "undefined"]
add_option( "sanitize", "enable selected sanitizer", 1, True,
            type="choice", choices=sanitizer_choices, default=None )

add_option( "durableDefaultOn" , "have durable default to on" , 0 , True )
add_option( "durableDefaultOff" , "have durable default to off" , 0 , True )

add_option( "pch" , "use precompiled headers to speed up the build (experimental)" , 0 , True , "usePCH" )
add_option( "distcc" , "use distcc for distributing builds" , 0 , False )

# debugging/profiling help
if os.sys.platform.startswith("linux") and (os.uname()[-1] == 'x86_64'):
    defaultAllocator = 'tcmalloc'
elif (os.sys.platform == "darwin") and (os.uname()[-1] == 'x86_64'):
    defaultAllocator = 'tcmalloc'
else:
    defaultAllocator = 'system'
add_option( "allocator" , "allocator to use (tcmalloc or system)" , 1 , True,
            default=defaultAllocator )
add_option( "gdbserver" , "build in gdb server support" , 0 , True )
add_option( "heapcheck", "link to heap-checking malloc-lib and look for memory leaks during tests" , 0 , False )
add_option( "gcov" , "compile with flags for gcov" , 0 , True )

add_option("smokedbprefix", "prefix to dbpath et al. for smoke tests", 1 , False )
add_option("smokeauth", "run smoke tests with --auth", 0 , False )

add_option("use-sasl-client", "Support SASL authentication in the client library", 0, False)

add_option( "use-system-tcmalloc", "use system version of tcmalloc library", 0, True )

add_option( "use-system-pcre", "use system version of pcre library", 0, True )

add_option( "use-system-boost", "use system version of boost libraries", 0, True )

add_option( "use-system-snappy", "use system version of snappy library", 0, True )

add_option( "use-system-v8", "use system version of v8 library", 0, True )

add_option( "use-system-stemmer", "use system version of stemmer", 0, True )

add_option( "use-system-all" , "use all system libraries", 0 , True )

add_option( "use-cpu-profiler",
            "Link against the google-perftools profiler library",
            0, False )

add_option("mongod-concurrency-level", "Concurrency level, \"global\" or \"db\"", 1, True,
           type="choice", choices=["global", "db"])

add_option('build-fast-and-loose', "NEVER for production builds", 0, False)

add_option('propagate-shell-environment',
           "Pass shell environment to sub-processes (NEVER for production builds)",
           0, False)

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

usev8 = has_option( "usev8" ) 

asio = has_option( "asio" )

usePCH = has_option( "usePCH" )

justClientLib = (COMMAND_LINE_TARGETS == ['mongoclient'])

env = Environment( BUILD_DIR=variantDir,
                   DIST_ARCHIVE_SUFFIX='.tgz',
                   EXTRAPATH=get_option("extrapath"),
                   MODULE_BANNERS=[],
                   MODULETEST_ALIAS='moduletests',
                   MODULETEST_LIST='#build/moduletests.txt',
                   MSVS_ARCH=msarch ,
                   PYTHON=utils.find_python(),
                   SERVER_ARCHIVE='${SERVER_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
                   TARGET_ARCH=msarch ,
                   tools=["default", "gch", "jsheader", "mergelib", "unittest"],
                   UNITTEST_ALIAS='unittests',
                   UNITTEST_LIST='#build/unittests.txt',
                   PYSYSPLATFORM=os.sys.platform,

                   PCRE_VERSION='8.30',
                   CONFIGUREDIR = '#' + scons_data_dir + '/sconf_temp',
                   CONFIGURELOG = '#' + scons_data_dir + '/config.log'
                   )

# This could be 'if solaris', but unfortuantely that variable hasn't been set yet.
if "sunos5" == os.sys.platform:
    # SERVER-9890: On Solaris, SCons preferentially loads the sun linker tool 'sunlink' when
    # using the 'default' tools as we do above. The sunlink tool sets -G as the flag for
    # creating a shared library. But we don't want that, since we always drive our link step
    # through CC or CXX. Instead, we want to let the compiler map GCC's '-shared' flag to the
    # appropriate linker specs that it has compiled in. We could (and should in the future)
    # select an empty set of tools above and then enable them as appropriate on a per platform
    # basis. Until then the simplest solution, as discussed on the scons-users mailing list,
    # appears to be to simply explicitly run the 'gnulink' tool to overwrite the Environment
    # changes made by 'sunlink'. See the following thread for more detail:
    #  http://four.pairlist.net/pipermail/scons-users/2013-June/001486.html
    env.Tool('gnulink')


if has_option("propagate-shell-environment"):
    env['ENV'] = dict(os.environ);

env['_LIBDEPS'] = '$_LIBDEPS_OBJS'

if has_option('build-fast-and-loose'):
    # See http://www.scons.org/wiki/GoFastButton for details
    env.Decider('MD5-timestamp')
    env.SetOption('max_drift', 1)
    env.SourceCode('.', None)

if has_option('mute'):
    env.Append( CCCOMSTR = "Compiling $TARGET" )
    env.Append( CXXCOMSTR = env["CCCOMSTR"] )
    env.Append( SHCCCOMSTR = "Compiling $TARGET" )
    env.Append( SHCXXCOMSTR = env["SHCCCOMSTR"] )
    env.Append( LINKCOMSTR = "Linking $TARGET" )
    env.Append( SHLINKCOMSTR = env["LINKCOMSTR"] )
    env.Append( ARCOMSTR = "Generating library $TARGET" )

if has_option('mongod-concurrency-level'):
    env.Append(CPPDEFINES=['MONGOD_CONCURRENCY_LEVEL=MONGOD_CONCURRENCY_LEVEL_%s' % get_option('mongod-concurrency-level').upper()])

libdeps.setup_environment( env )

if env['PYSYSPLATFORM'] == 'linux3':
    env['PYSYSPLATFORM'] = 'linux2'
if 'freebsd' in env['PYSYSPLATFORM']:
    env['PYSYSPLATFORM'] = 'freebsd'

if os.sys.platform == 'win32':
    env['OS_FAMILY'] = 'win'
else:
    env['OS_FAMILY'] = 'posix'

if has_option( "cc-use-shell-environment" ) and has_option( "cc" ):
    print("Cannot specify both --cc-use-shell-environment and --cc")
    Exit(1)
elif has_option( "cxx-use-shell-environment" ) and has_option( "cxx" ):
    print("Cannot specify both --cxx-use-shell-environment and --cxx")
    Exit(1)

if has_option( "cxx-use-shell-environment" ):
    env["CXX"] = os.getenv("CXX");
    env["CC"] = env["CXX"]
if has_option( "cc-use-shell-environment" ):
    env["CC"] = os.getenv("CC");

if has_option( "cxx" ):
    env["CC"] = get_option( "cxx" )
    env["CXX"] = get_option( "cxx" )
if has_option( "cc" ):
    env["CC"] = get_option( "cc" )

if has_option( "ld" ):
    env["LINK"] = get_option( "ld" )

if env['PYSYSPLATFORM'] in ('linux2', 'freebsd'):
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

if ( not ( usev8 or justClientLib) ):
    usev8 = True
    options_topass["usev8"] = True

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

def filterExists(paths):
    return filter(os.path.exists, paths)

if "darwin" == os.sys.platform:
    darwin = True
    platform = "osx" # prettier than darwin

    # Unfortunately, we are too late here to affect the variant dir. We could maybe make this
    # flag available on all platforms and complain if it is used on non-darwin targets.
    osx_version_choices = ['10.6', '10.7', '10.8']
    add_option("osx-version-min", "minimum OS X version to support", 1, False,
               type = 'choice', default = osx_version_choices[0], choices = osx_version_choices)

    if env["CXX"] is None:
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
        env.Append( CCFLAGS=["-mmmx"] )

    nix = True

    if static:
        env.Append( LINKFLAGS=" -static " )
    if has_option( "static-libstdc++" ):
        env.Append( LINKFLAGS=" -static-libstdc++ " )

elif "sunos5" == os.sys.platform:
     nix = True
     solaris = True
     env.Append( CPPDEFINES=[ "__sunos__" ] )
     env.Append( LIBS=["socket","resolv"] )

elif os.sys.platform.startswith( "freebsd" ):
    nix = True
    freebsd = True
    env.Append( LIBS=[ "kvm" ] )
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__freebsd__" ] )
    env.Append( CCFLAGS=[ "-fno-omit-frame-pointer" ] )

elif os.sys.platform.startswith( "openbsd" ):
    nix = True
    openbsd = True
    env.Append( EXTRACPPPATH=[ "/usr/local/include" ] )
    env.Append( EXTRALIBPATH=[ "/usr/local/lib" ] )
    env.Append( CPPDEFINES=[ "__openbsd__" ] )

elif "win32" == os.sys.platform:
    windows = True
    dynamicCRT = has_option("dynamic-windows")
    
    env['DIST_ARCHIVE_SUFFIX'] = '.zip'

    win_version_min_choices = {
        'xpsp3'   : ('0501', '0300'),
        'ws03sp2' : ('0502', '0200'),
        'vista'   : ('0600', '0000'),
        'ws08r2'  : ('0601', '0000'),
        'win7'    : ('0601', '0000'),
        'win8'    : ('0602', '0000'),
    }

    add_option("win-version-min", "minimum Windows version to support", 1, False,
               type = 'choice', default = None,
               choices = win_version_min_choices.keys())

    if has_option('win-version-min') and has_option('win2008plus'):
        print("Can't specify both 'win-version-min' and 'win2008plus'")
        Exit(1)

    # If tools configuration fails to set up 'cl' in the path, fall back to importing the whole
    # shell environment and hope for the best. This will work, for instance, if you have loaded
    # an SDK shell.
    for pathdir in env['ENV']['PATH'].split(os.pathsep):
        if os.path.exists(os.path.join(pathdir, 'cl.exe')):
            break
    else:
        print("NOTE: Tool configuration did not find 'cl' compiler, falling back to os environment")
        env['ENV'] = dict(os.environ)

    env.Append( CPPDEFINES=[ "_UNICODE" ] )
    env.Append( CPPDEFINES=[ "UNICODE" ] )

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
    env.Append( CPPDEFINES=["_CONSOLE","_CRT_SECURE_NO_WARNINGS"] )

    # this would be for pre-compiled headers, could play with it later  
    #env.Append( CCFLAGS=['/Yu"pch.h"'] )

    # docs say don't use /FD from command line (minimal rebuild)
    # /Gy function level linking (implicit when using /Z7)
    # /Z7 debug info goes into each individual .obj file -- no .pdb created 
    env.Append( CCFLAGS= ["/Z7", "/errorReport:none"] )
    if release:
        # /O2:  optimize for speed (as opposed to size)
        # /Oy-: disable frame pointer optimization (overrides /O2, only affects 32-bit)
        env.Append( CCFLAGS= ["/O2", "/Oy-"] )

        # /DEBUG will tell the linker to create a .pdb file
        # which WinDbg and Visual Studio will use to resolve
        # symbols if you want to debug a release-mode image.
        # Note that this means we can't do parallel links in the build.
        env.Append( LINKFLAGS=" /DEBUG " )

        # /MD:  use the multithreaded, DLL version of the run-time library (MSVCRT.lib/MSVCR###.DLL)
        # /MT:  use the multithreaded, static version of the run-time library (LIBCMT.lib)
        if dynamicCRT:
            env.Append( CCFLAGS= ["/MD"] )
        else:
            env.Append( CCFLAGS= ["/MT"] )
    else:
        # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized
        #        (implies /Od: no optimizations)
        env.Append( CCFLAGS=["/RTC1", "/Od"] )
        if debugBuild:
            # If you build without --d, no debug PDB will be generated, and 
            # linking will be faster. However, you won't be able to debug your code with the debugger.
            env.Append( LINKFLAGS=" /debug " )
        # /MDd: Defines _DEBUG, _MT, _DLL, and uses MSVCRTD.lib/MSVCRD###.DLL
        # /MTd: Defines _DEBUG, _MT, and causes your application to use the
        #       debug multithread version of the run-time library (LIBCMTD.lib)
        if dynamicCRT:
            env.Append( CCFLAGS= ["/MDd"] )
        else:
            env.Append( CCFLAGS= ["/MTd"] )
    # This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds
    env.Append( LINKFLAGS=" /LARGEADDRESSAWARE " )

    env.Append(LIBS=['ws2_32.lib', 'kernel32.lib', 'advapi32.lib', 'Psapi.lib', 'DbgHelp.lib', 'shell32.lib'])

    # v8 calls timeGetTime()
    if usev8:
        env.Append(LIBS=['winmm.lib'])

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
                         "-ggdb",
                         "-pthread",
                         "-Wall",
                         "-Wsign-compare",
                         "-Wno-unknown-pragmas",
                         "-Winvalid-pch"] )
    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    if linux or darwin:
        env.Append( CCFLAGS=["-Werror", "-pipe"] )

    env.Append( CPPDEFINES=["_FILE_OFFSET_BITS=64"] )
    env.Append( CXXFLAGS=["-Wnon-virtual-dtor", "-Woverloaded-virtual"] )
    env.Append( LINKFLAGS=["-fPIC", "-pthread"] )

    # SERVER-9761: Ensure early detection of missing symbols in dependent libraries at program
    # startup.
    #
    # TODO: Is it necessary to add to both linkflags and shlinkflags, or are LINKFLAGS
    # propagated to SHLINKFLAGS?
    if darwin:
        env.Append( LINKFLAGS=["-Wl,-bind_at_load"] )
        env.Append( SHLINKFLAGS=["-Wl,-bind_at_load"] )
    else:
        env.Append( LINKFLAGS=["-Wl,-z,now"] )
        env.Append( SHLINKFLAGS=["-Wl,-z,now"] )

    if not darwin:
        env.Append( LINKFLAGS=["-rdynamic"] )

    env.Append( LIBS=[] )

    #make scons colorgcc friendly
    for key in ('HOME', 'TERM'):
        try:
            env['ENV'][key] = os.environ[key]
        except KeyError:
            pass

    if linux and has_option( "gcov" ):
        env.Append( CXXFLAGS=" -fprofile-arcs -ftest-coverage " )
        env.Append( LINKFLAGS=" -fprofile-arcs -ftest-coverage " )

    if debugBuild:
        env.Append( CCFLAGS=["-O0", "-fstack-protector"] )
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

if "uname" in dir(os):
    hacks = buildscripts.findHacks( os.uname() )
    if hacks is not None:
        hacks.insert( env , { "linux64" : linux64 } )

if has_option( "ssl" ):
    env.Append( CPPDEFINES=["MONGO_SSL"] )
    if windows:
        env.Append( LIBS=["libeay32"] )
        env.Append( LIBS=["ssleay32"] )
    else:
        env.Append( LIBS=["ssl"] )
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

env.Prepend(CPPPATH=['$BUILD_DIR/third_party/s2'])

if not use_system_version_of_library("stemmer"):
    env.Prepend(CPPPATH=['$BUILD_DIR/third_party/libstemmer_c/include'])

if not use_system_version_of_library("snappy"):
    env.Prepend(CPPPATH=['$BUILD_DIR/third_party/snappy'])

env.Append( CPPPATH=['$EXTRACPPPATH'],
            LIBPATH=['$EXTRALIBPATH'] )

# discover modules, and load the (python) module for each module's build.py
mongo_modules = moduleconfig.discover_modules('src/mongo/db/modules')
env['MONGO_MODULES'] = [m.name for m in mongo_modules]

# --- check system ---

def doConfigure(myenv):

    # Check that the compilers work.
    #
    # TODO: Currently, we have some flags already injected. Eventually, this should test the
    # bare compilers, and we should re-check at the very end that TryCompile and TryLink still
    # work with the flags we have selected.
    conf = Configure(myenv, help=False)

    if 'CheckCXX' in dir( conf ):
        if not conf.CheckCXX():
            print("C++ compiler %s does not work" % (conf.env["CXX"]))
            Exit(1)

    # Only do C checks if CC != CXX
    check_c = (myenv["CC"] != myenv["CXX"])

    if check_c and 'CheckCC' in dir( conf ):
        if not conf.CheckCC():
            print("C compiler %s does not work" % (conf.env["CC"]))
            Exit(1)
    myenv = conf.Finish()

    # Identify the toolchain in use. We currently support the following:
    # TODO: Goes in the env?
    toolchain_gcc = "GCC"
    toolchain_clang = "clang"
    toolchain_msvc = "MSVC"

    def CheckForToolchain(context, toolchain, lang_name, compiler_var, source_suffix):
        test_bodies = {
            toolchain_gcc : (
                # Clang also defines __GNUC__
                """
                #if !defined(__GNUC__) || defined(__clang__)
                #error
                #endif
                """),
            toolchain_clang : (
                """
                #if !defined(__clang__)
                #error
                #endif
                """),
            toolchain_msvc : (
                """
                #if !defined(_MSC_VER)
                #error
                #endif
                """),
        }
        print_tuple = (lang_name, context.env[compiler_var], toolchain)
        context.Message('Checking if %s compiler "%s" is %s... ' % print_tuple)
        # Strip indentation from the test body to ensure that the newline at the end of the
        # endif is the last character in the file (rather than a line of spaces with no
        # newline), and that all of the preprocessor directives start at column zero. Both of
        # these issues can trip up older toolchains.
        test_body = textwrap.dedent(test_bodies[toolchain])
        result = context.TryCompile(test_body, source_suffix)
        context.Result(result)
        return result

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckForToolchain' : CheckForToolchain,
    })

    toolchain = None
    have_toolchain = lambda: toolchain != None
    using_msvc = lambda: toolchain == toolchain_msvc
    using_gcc = lambda: toolchain == toolchain_gcc
    using_clang = lambda: toolchain == toolchain_clang

    if windows:
        toolchain_search_sequence = [toolchain_msvc]
    else:
        toolchain_search_sequence = [toolchain_gcc, toolchain_clang]

    for candidate_toolchain in toolchain_search_sequence:
        if conf.CheckForToolchain(candidate_toolchain, "C++", "CXX", ".cpp"):
            toolchain = candidate_toolchain
            break

    if not have_toolchain():
        print("Couldn't identify the toolchain")
        Exit(1)

    if check_c and not conf.CheckForToolchain(toolchain, "C", "CC", ".c"):
        print("C toolchain doesn't match identified C++ toolchain")
        Exit(1)

    myenv = conf.Finish()

    # Figure out what our minimum windows version is. If the user has specified, then use
    # that. Otherwise, if they have explicitly selected between 32 bit or 64 bit, choose XP or
    # Vista respectively. Finally, if they haven't done either of these, try invoking the
    # compiler to figure out whether we are doing a 32 or 64 bit build and select as
    # appropriate.
    if windows:
        win_version_min = None
        default_32_bit_min = 'xpsp3'
        default_64_bit_min = 'ws03sp2'
        if has_option('win-version-min'):
            win_version_min = get_option('win-version-min')
        elif has_option('win2008plus'):
            win_version_min = 'win7'
        else:
            if force32:
                win_version_min = default_32_bit_min
            elif force64:
                win_version_min = default_64_bit_min
            else:
                def CheckFor64Bit(context):
                    win64_test_body = textwrap.dedent(
                        """
                        #if !defined(_WIN64)
                        #error
                        #endif
                        """
                    )
                    context.Message('Checking if toolchain is in 64-bit mode... ')
                    result = context.TryCompile(win64_test_body, ".c")
                    context.Result(result)
                    return result

                conf = Configure(myenv, help=False, custom_tests = {
                    'CheckFor64Bit' : CheckFor64Bit
                })
                if conf.CheckFor64Bit():
                    win_version_min = default_64_bit_min
                else:
                    win_version_min = default_32_bit_min
                conf.Finish();

        win_version_min = win_version_min_choices[win_version_min]
        env.Append( CPPDEFINES=[("_WIN32_WINNT", "0x" + win_version_min[0])] )
        env.Append( CPPDEFINES=[("NTDDI_VERSION", "0x" + win_version_min[0] + win_version_min[1])] )

    # Enable PCH if we are on using gcc or clang and the 'Gch' tool is enabled. Otherwise,
    # remove any pre-compiled header since the compiler may try to use it if it exists.
    if usePCH and (using_gcc() or using_clang()):
        if 'Gch' in dir( myenv ):
            if using_clang():
                # clang++ uses pch.h.pch rather than pch.h.gch
                myenv['GCHSUFFIX'] = '.pch'
                # clang++ only uses pch from command line
                myenv.Prepend( CXXFLAGS=['-include pch.h'] )
            myenv['Gch'] = myenv.Gch( "$BUILD_DIR/mongo/pch.h$GCHSUFFIX",
                                        "src/mongo/pch.h" )[0]
            myenv['GchSh'] = myenv[ 'Gch' ]
    elif os.path.exists( myenv.File("$BUILD_DIR/mongo/pch.h$GCHSUFFIX").abspath ):
        print( "removing precompiled headers" )
        os.unlink( myenv.File("$BUILD_DIR/mongo/pch.h.$GCHSUFFIX").abspath )

    def AddFlagIfSupported(env, tool, extension, flag, **mutation):
        def CheckFlagTest(context, tool, extension, flag):
            test_body = ""
            context.Message('Checking if %s compiler supports %s... ' % (tool, flag))
            ret = context.TryCompile(test_body, extension)
            context.Result(ret)
            return ret

        if using_msvc():
            print("AddFlagIfSupported is not currently supported with MSVC")
            Exit(1)

        test_mutation = mutation
        if using_gcc():
            test_mutation = copy.deepcopy(mutation)
            # GCC helpfully doesn't issue a diagnostic on unkown flags of the form -Wno-xxx
            # unless other diagnostics are triggered. That makes it tough to check for support
            # for -Wno-xxx. To work around, if we see that we are testing for a flag of the
            # form -Wno-xxx (but not -Wno-error=xxx), we also add -Wxxx to the flags. GCC does
            # warn on unknown -Wxxx style flags, so this lets us probe for availablity of
            # -Wno-xxx.
            for kw in test_mutation.keys():
                test_flags = test_mutation[kw]
                for test_flag in test_flags:
                    if test_flag.startswith("-Wno-") and not test_flag.startswith("-Wno-error="):
                        test_flags.append(re.sub("^-Wno-", "-W", test_flag))

        cloned = env.Clone()
        cloned.Append(**test_mutation)

        # For GCC, we don't need anything since bad flags are already errors, but
        # adding -Werror won't hurt. For clang, bad flags are only warnings, so we need -Werror
        # to make them real errors.
        cloned.Append(CCFLAGS=['-Werror'])
        conf = Configure(cloned, help=False, custom_tests = {
                'CheckFlag' : lambda(ctx) : CheckFlagTest(ctx, tool, extension, flag)
        })
        available = conf.CheckFlag()
        conf.Finish()
        if available:
            env.Append(**mutation)
        return available

    def AddToCFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, CFLAGS=[flag])

    def AddToCCFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, CCFLAGS=[flag])

    def AddToCXXFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C++', '.cpp', flag, CXXFLAGS=[flag])

    if using_gcc() or using_clang():
        # This warning was added in g++-4.8.
        AddToCCFLAGSIfSupported(myenv, '-Wno-unused-local-typedefs')

        # Clang likes to warn about unused functions, which seems a tad aggressive and breaks
        # -Werror, which we want to be able to use.
        AddToCCFLAGSIfSupported(myenv, '-Wno-unused-function')

        # TODO: Note that the following two flags are added to CCFLAGS even though they are
        # really C++ specific. We need to do this because SCons passes CXXFLAGS *before*
        # CCFLAGS, but CCFLAGS contains -Wall, which re-enables the warnings we are trying to
        # suppress. In the future, we should move all warning flags to CCWARNFLAGS and
        # CXXWARNFLAGS and add these to CCOM and CXXCOM as appropriate.
        #
        # Clang likes to warn about unused private fields, but some of our third_party
        # libraries have such things.
        AddToCCFLAGSIfSupported(myenv, '-Wno-unused-private-field')

        # Clang warns about struct/class tag mismatch, but most people think that that is not
        # really an issue, see
        # http://stackoverflow.com/questions/4866425/mixing-class-and-struct. We disable the
        # warning so it doesn't become an error.
        AddToCCFLAGSIfSupported(myenv, '-Wno-mismatched-tags')

        # Prevents warning about using deprecated features (such as auto_ptr in c++11)
        # Using -Wno-error=deprecated-declarations does not seem to work on some compilers,
        # including at least g++-4.6.
        AddToCCFLAGSIfSupported(myenv, "-Wno-deprecated-declarations")

        # As of clang-3.4, this warning appears in v8, and gets escalated to an error.
        AddToCCFLAGSIfSupported(myenv, "-Wno-tautological-constant-out-of-range-compare")

    if has_option('c++11'):
        # The Microsoft compiler does not need a switch to enable C++11. Again we should be
        # checking for MSVC, not windows. In theory, we might be using clang or icc on windows.
        if not using_msvc():
            # For our other compilers (gcc and clang) we need to pass -std=c++0x or -std=c++11,
            # but we prefer the latter. Try that first, and fall back to c++0x if we don't
            # detect that --std=c++11 works.
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++11'):
                if not AddToCXXFLAGSIfSupported(myenv, '-std=c++0x'):
                    print( 'C++11 mode requested, but cannot find a flag to enable it' )
                    Exit(1)
            # Our current builtin tcmalloc is not compilable in C++11 mode. Remove this
            # check when our builtin release of tcmalloc contains the resolution to
            # http://code.google.com/p/gperftools/issues/detail?id=477.
            if get_option('allocator') == 'tcmalloc':
                if not use_system_version_of_library('tcmalloc'):
                    print( 'TCMalloc is not currently compatible with C++11' )
                    Exit(1)

            if not AddToCFLAGSIfSupported(myenv, '-std=c99'):
                print( 'C++11 mode selected for C++ files, but failed to enable C99 for C files' )

    # This needs to happen before we check for libc++, since it affects whether libc++ is available.
    if darwin and has_option('osx-version-min'):
        min_version = get_option('osx-version-min')
        if not AddToCCFLAGSIfSupported(myenv, '-mmacosx-version-min=%s' % (min_version)):
            print( "Can't set minimum OS X version with this compiler" )
            Exit(1)

    if has_option('libc++'):
        if not using_clang():
            print( 'libc++ is currently only supported for clang')
            Exit(1)
        if AddToCXXFLAGSIfSupported(myenv, '-stdlib=libc++'):
            myenv.Append(LINKFLAGS=['-stdlib=libc++'])
        else:
            print( 'libc++ requested, but compiler does not support -stdlib=libc++' )
            Exit(1)

    # Check to see if we are trying to use an outdated libstdc++ in C++11 mode. This is
    # primarly to help people using clang in C++11 mode on OS X but forgetting to use
    # --libc++. We would, ideally, check the __GLIBCXX__ version, but for various reasons this
    # is not workable. Instead, we switch on the fact that std::is_nothrow_constructible wasn't
    # introduced until libstdc++ 4.6.0. Earlier versions of libstdc++ than 4.6 are unlikely to
    # work well anyway.
    if has_option('c++11') and not has_option('libc++'):

        def CheckModernLibStdCxx(context):

            test_body = """
            #include <vector>
            #include <cstdlib>
            #if defined(__GLIBCXX__)
            #include <type_traits>
            int main() {
                return std::is_nothrow_constructible<int>::value ? EXIT_SUCCESS : EXIT_FAILURE;
            }
            #endif
            """

            context.Message('Checking for libstdc++ 4.6.0 or better (for C++11 support)... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckModernLibStdCxx' : CheckModernLibStdCxx,
        })
        haveGoodLibStdCxx = conf.CheckModernLibStdCxx()
        conf.Finish()

        if not haveGoodLibStdCxx:
            print( 'Detected libstdc++ is too old to support C++11 mode' )
            if darwin:
                print( 'Try building with --libc++ and --osx-version-min=10.7 or higher' )
            Exit(1)

    if has_option('sanitize'):
        if not (using_clang() or using_gcc()):
            print( 'sanitize is only supported with clang or gcc')
            Exit(1)
        sanitizer_option = '-fsanitize=' + GetOption('sanitize')
        if AddToCCFLAGSIfSupported(myenv, sanitizer_option):
            myenv.Append(LINKFLAGS=[sanitizer_option])
            myenv.Append(CCFLAGS=['-fno-omit-frame-pointer'])
        else:
            print( 'Failed to enable sanitizer with flag: ' + sanitizer_option )
            Exit(1)

    # Apply any link time optimization settings as selected by the 'lto' option.
    if has_option('lto'):
        if using_msvc():
            # Note that this is actually more aggressive than LTO, it is whole program
            # optimization due to /GL. However, this is historically what we have done for
            # windows, so we are keeping it.
            #
            # /GL implies /LTCG, so no need to say it in CCFLAGS, but we do need /LTCG on the
            # link flags.
            myenv.Append(CCFLAGS=['/GL'])
            myenv.Append(LINKFLAGS=['/LTCG'])
            myenv.Append(ARFLAGS=['/LTCG'])
        elif using_gcc() or using_clang():
            # For GCC and clang, the flag is -flto, and we need to pass it both on the compile
            # and link lines.
            if AddToCCFLAGSIfSupported(myenv, '-flto'):
                myenv.Append(LINKFLAGS=['-flto'])
            else:
                print( "Link time optimization requested, " +
                       "but selected compiler does not honor -flto" )
                Exit(1)
        else:
            printf("Don't know how to enable --lto on current toolchain")
            Exit(1)

    # glibc's memcmp is faster than gcc's
    if linux:
        AddToCCFLAGSIfSupported(myenv, "-fno-builtin-memcmp")

    conf = Configure(myenv)
    libdeps.setup_conftests(conf)

    if use_system_version_of_library("pcre"):
        conf.FindSysLibDep("pcre", ["pcre"])
        conf.FindSysLibDep("pcrecpp", ["pcrecpp"])

    if use_system_version_of_library("snappy"):
        conf.FindSysLibDep("snappy", ["snappy"])

    if use_system_version_of_library("stemmer"):
        conf.FindSysLibDep("stemmer", ["stemmer"])

    if use_system_version_of_library("boost"):
        if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
            print( "can't find boost headers" )
            Exit(1)

        for b in boostLibs:
            l = "boost_" + b
            conf.FindSysLibDep(l,
                [ l + boostCompiler + "-mt" + boostVersion,
                  l + boostCompiler + boostVersion ], language='C++' )

    if conf.CheckHeader('unistd.h'):
        conf.env.Append(CPPDEFINES=['MONGO_HAVE_HEADER_UNISTD_H'])

    if solaris or conf.CheckDeclaration('clock_gettime', includes='#include <time.h>'):
        conf.CheckLib('rt')

    if (conf.CheckCXXHeader( "execinfo.h" ) and
        conf.CheckDeclaration('backtrace', includes='#include <execinfo.h>') and
        conf.CheckDeclaration('backtrace_symbols', includes='#include <execinfo.h>') and
        conf.CheckDeclaration('backtrace_symbols_fd', includes='#include <execinfo.h>')):

        conf.env.Append( CPPDEFINES=[ "MONGO_HAVE_EXECINFO_BACKTRACE" ] )

    conf.env["_HAVEPCAP"] = conf.CheckLib( ["pcap", "wpcap"], autoadd=False )

    if solaris:
        conf.CheckLib( "nsl" )

    if usev8 and use_system_version_of_library("v8"):
        if debugBuild:
            v8_lib_choices = ["v8_g", "v8"]
        else:
            v8_lib_choices = ["v8"]
        conf.FindSysLibDep( "v8", v8_lib_choices )

    conf.env['MONGO_BUILD_SASL_CLIENT'] = bool(has_option("use-sasl-client"))
    if conf.env['MONGO_BUILD_SASL_CLIENT'] and not conf.CheckLibWithHeader(
        "sasl2", "sasl/sasl.h", "C", "sasl_version_info(0, 0, 0, 0, 0, 0);", autoadd=False ):
        Exit(1)

    # requires ports devel/libexecinfo to be installed
    if freebsd or openbsd:
        if not conf.CheckLib("execinfo"):
            Exit(1)

    # 'tcmalloc' needs to be the last library linked. Please, add new libraries before this 
    # point.
    if get_option('allocator') == 'tcmalloc':
        if use_system_version_of_library('tcmalloc'):
            conf.FindSysLibDep("tcmalloc", ["tcmalloc"])
        elif has_option("heapcheck"):
            print ("--heapcheck does not work with the tcmalloc embedded in the mongodb source "
                   "tree.  Use --use-system-tcmalloc.")
            Exit(1)
    elif get_option('allocator') == 'system':
        pass
    else:
        print "Invalid --allocator parameter: \"%s\"" % get_option('allocator')
        Exit(1)

    if has_option("heapcheck"):
        if ( not debugBuild ) and ( not debugLogging ):
            print( "--heapcheck needs --d or --dd" )
            Exit( 1 )

        if not conf.CheckCXXHeader( "google/heap-checker.h" ):
            print( "--heapcheck neads header 'google/heap-checker.h'" )
            Exit( 1 )

        conf.env.Append( CPPDEFINES=[ "HEAP_CHECKING" ] )
        conf.env.Append( CCFLAGS=["-fno-omit-frame-pointer"] )

    # ask each module to configure itself and the build environment.
    moduleconfig.configure_modules(mongo_modules, conf)

    return conf.Finish()

env = doConfigure( env )

env['PDB'] = '${TARGET.base}.pdb'

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

enforce_glibc = linux and has_option("release") and not has_option("no-glibc-check")

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

# --- lint ----



def doLint( env , target , source ):
    import buildscripts.lint
    if not buildscripts.lint.run_lint( [ "src/mongo/" ] ):
        raise Exception( "lint errors" )

env.Alias( "lint" , [] , [ doLint ] )
env.AlwaysBuild( "lint" )


#  ----  INSTALL -------

def getSystemInstallName():
    n = platform + "-" + processor
    if static:
        n += "-static"
    if has_option("nostrip"):
        n += "-debugsymbols"
    if nix and os.uname()[2].startswith("8."):
        n += "-tiger"

    if len(mongo_modules):
            n += "-" + "-".join(m.name for m in mongo_modules)

    try:
        findSettingsSetup()
        import settings
        if "distmod" in dir(settings):
            n = n + "-" + str(settings.distmod)
    except:
        pass

    dn = GetOption("distmod")
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

def s3push(localName, remoteName=None, platformDir=True):
    localName = str( localName )

    if isBuildingLatest:
        remotePrefix = utils.getGitBranchString("-") + "-latest"
    else:
        remotePrefix = "-" + distName

    findSettingsSetup()

    import simples3
    import settings

    s = simples3.S3Bucket( settings.bucket , settings.id , settings.key )

    if remoteName is None:
        remoteName = localName

    name = '%s-%s%s' % (remoteName , getSystemInstallName(), remotePrefix)
    lastDotIndex = localName.rfind('.')
    if lastDotIndex != -1:
        name += localName[lastDotIndex:]
    name = name.lower()

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

env.Alias( "dist" , '$SERVER_ARCHIVE' )
env.AlwaysBuild(env.Alias( "s3dist" , [ '$SERVER_ARCHIVE' ] , [ s3dist ] ))

# --- an uninstall target ---
if len(COMMAND_LINE_TARGETS) > 0 and 'uninstall' in COMMAND_LINE_TARGETS:
    SetOption("clean", 1)
    # By inspection, changing COMMAND_LINE_TARGETS here doesn't do
    # what we want, but changing BUILD_TARGETS does.
    BUILD_TARGETS.remove("uninstall")
    BUILD_TARGETS.append("install")

module_sconscripts = moduleconfig.get_module_sconscripts(mongo_modules)

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
Export("has_option use_system_version_of_library")
Export("installSetup")
Export("usev8")
Export("darwin windows solaris linux freebsd nix")
Export('module_sconscripts')
Export("debugBuild")
Export("enforce_glibc")

env.SConscript('src/SConscript', variant_dir='$BUILD_DIR', duplicate=False)
env.SConscript('src/SConscript.client', variant_dir='$BUILD_DIR/client_build', duplicate=False)
env.SConscript(['SConscript.buildinfo', 'SConscript.smoke'])

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

env.Alias('all', ['core', 'tools', 'clientTests', 'test', 'unittests', 'moduletests'])
