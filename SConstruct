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
import shlex
import shutil
import stat
import sys
import textwrap
import types
import urllib
import urllib2
import uuid
from buildscripts import utils
from buildscripts import moduleconfig

import libdeps

EnsureSConsVersion( 2, 3, 0 )

def findSettingsSetup():
    sys.path.append( "." )
    sys.path.append( ".." )
    sys.path.append( "../../" )

def versiontuple(v):
    return tuple(map(int, (v.split("."))))

# --- platform identification ---
#
# This needs to precede the options section so that we can only offer some options on certain
# platforms.

platform = os.sys.platform
nix = False
linux = False
darwin = False
windows = False
freebsd = False
openbsd = False
solaris = False

if "darwin" == platform:
    darwin = True
    platform = "osx" # prettier than darwin
elif platform.startswith("linux"):
    linux = True
    platform = "linux"
elif "sunos5" == platform:
    solaris = True
elif platform.startswith( "freebsd" ):
    freebsd = True
elif platform.startswith( "openbsd" ):
    openbsd = True
elif "win32" == platform:
    windows = True
else:
    print( "No special config for [" + platform + "] which probably means it won't work" )

nix = not windows

# --- options ----
options = {}

def add_option( name, help, nargs, contributesToVariantDir,
                dest=None, default = None, type="string", choices=None, metavar=None, const=None ):

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
               const=const,
               help=help )

    options[name] = { "help" : help ,
                      "nargs" : nargs ,
                      "contributesToVariantDir" : contributesToVariantDir ,
                      "dest" : dest,
                      "default": default }

def get_option( name ):
    return GetOption( name )

def has_option( name ):
    x = get_option( name )
    if x is None:
        return False

    if x == False:
        return False

    if x == "":
        return False

    return True

def use_system_version_of_library(name):
    return has_option('use-system-all') or has_option('use-system-' + name)

# Returns true if we have been configured to use a system version of any C++ library. If you
# add a new C++ library dependency that may be shimmed out to the system, add it to the below
# list.
def using_system_version_of_cxx_libraries():
    cxx_library_names = ["tcmalloc", "boost", "v8"]
    return True in [use_system_version_of_library(x) for x in cxx_library_names]

def get_variant_dir():

    build_dir = get_option('build-dir').rstrip('/')

    if has_option('variant-dir'):
        return (build_dir + '/' + get_option('variant-dir')).rstrip('/')

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

    extras = []
    if has_option("extra-variant-dirs"):
        extras = [substitute(x) for x in get_option( 'extra-variant-dirs' ).split( ',' )]

    if has_option("add-branch-to-variant-dir"):
        extras += ["branch_" + substitute( utils.getGitBranch() )]

    if has_option('cache'):
        s = "cached"
        s += "/".join(extras) + "/"
    else:
        s = "${PYSYSPLATFORM}/${TARGET_ARCH}/"
        a += extras

        if len(a) > 0:
            a.sort()
            s += "/".join( a ) + "/"
        else:
            s += "normal/"

    return (build_dir + '/' + s).rstrip('/')

# build output
add_option( "mute" , "do not display commandlines for compiling and linking, to reduce screen noise", 0, False )

# installation/packaging
add_option( "prefix" , "installation prefix" , 1 , False, default='$BUILD_ROOT/install' )
add_option( "distname" , "dist name (0.8.0)" , 1 , False )
add_option( "distmod", "additional piece for full dist name" , 1 , False )
add_option( "distarch", "override the architecture name in dist output" , 1 , False )
add_option( "nostrip", "do not strip installed binaries" , 0 , False )
add_option( "extra-variant-dirs", "extra variant dir components, separated by commas", 1, False)
add_option( "add-branch-to-variant-dir", "add current git branch to the variant dir", 0, False )
add_option( "build-dir", "build output directory", 1, False, default='#build')
add_option( "variant-dir", "override variant subdirectory", 1, False )

# linking options
add_option( "release" , "release build" , 0 , True )
add_option( "static-libstdc++" , "statically link libstdc++" , 0 , False )
add_option( "lto", "enable link time optimizations (experimental, except with MSVC)" , 0 , True )
add_option( "dynamic-windows", "dynamically link on Windows", 0, True)

# base compile flags
add_option( "endian" , "endianness of target platform" , 1 , False , "endian",
            type="choice", choices=["big", "little", "auto"], default="auto" )

add_option( "cxx", "compiler to use" , 1 , True )
add_option( "cc", "compiler to use for c" , 1 , True )
add_option( "cc-use-shell-environment", "use $CC from shell for C compiler" , 0 , False )
add_option( "cxx-use-shell-environment", "use $CXX from shell for C++ compiler" , 0 , False )
add_option( "disable-minimum-compiler-version-enforcement",
            "allow use of unsupported older compilers (NEVER for production builds)",
            0, False )

add_option( "ssl" , "Enable SSL" , 0 , True )
add_option( "ssl-fips-capability", "Enable the ability to activate FIPS 140-2 mode", 0, True );
add_option( "wiredtiger", "Enable wiredtiger", "?", True, "wiredtiger",
            type="choice", choices=["on", "off"], const="on", default="on")

# library choices
js_engine_choices = ['v8-3.12', 'v8-3.25', 'none']
add_option( "js-engine", "JavaScript scripting engine implementation", 1, False,
           type='choice', default=js_engine_choices[0], choices=js_engine_choices)
add_option( "server-js", "Build mongod without JavaScript support", 1, False,
           type='choice', choices=["on", "off"], const="on", default="on")
add_option( "libc++", "use libc++ (experimental, requires clang)", 0, True )

add_option( "use-glibcxx-debug",
            "Enable the glibc++ debug implementations of the C++ standard libary", 0, True )

# mongo feature options
add_option( "noshell", "don't build shell" , 0 , True )
add_option( "safeshell", "don't let shell scripts run programs (still, don't run untrusted scripts)" , 0 , True )

# new style debug and optimize flags
add_option( "dbg", "Enable runtime debugging checks", "?", True, "dbg",
            type="choice", choices=["on", "off"], const="on" )

add_option( "opt", "Enable compile-time optimization", "?", True, "opt",
            type="choice", choices=["on", "off"], const="on" )

add_option( "sanitize", "enable selected sanitizers", 1, True, metavar="san1,san2,...sanN" )
add_option( "llvm-symbolizer", "name of (or path to) the LLVM symbolizer", 1, False, default="llvm-symbolizer" )

add_option( "durableDefaultOn" , "have durable default to on" , 0 , True )
add_option( "durableDefaultOff" , "have durable default to off" , 0 , True )

# debugging/profiling help
if os.sys.platform.startswith("linux"):
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

add_option( "use-system-wiredtiger", "use system version of wiredtiger library", 0, True)

# library choices
boost_choices = ['1.56']
add_option( "internal-boost", "Specify internal boost version to use", 1, True,
           type='choice', default=boost_choices[0], choices=boost_choices)

add_option( "system-boost-lib-search-suffixes",
            "Comma delimited sequence of boost library suffixes to search",
            1, False )

add_option( "use-system-boost", "use system version of boost libraries", 0, True )

add_option( "use-system-snappy", "use system version of snappy library", 0, True )

add_option( "use-system-zlib", "use system version of zlib library", 0, True )

add_option( "use-system-v8", "use system version of v8 library", 0, True )

add_option( "use-system-stemmer", "use system version of stemmer", 0, True )

add_option( "use-system-yaml", "use system version of yaml", 0, True )

add_option( "use-system-all" , "use all system libraries", 0 , True )

# deprecated
add_option( "use-new-tools" , "put new tools in the tarball", 0 , False )

add_option( "use-cpu-profiler",
            "Link against the google-perftools profiler library",
            0, False )

add_option('build-fast-and-loose', "looser dependency checking, ignored for --release builds",
           '?', False, type="choice", choices=["on", "off"], const="on", default="on")

add_option('disable-warnings-as-errors', "Don't add -Werror to compiler command line", 0, False)

add_option('propagate-shell-environment',
           "Pass shell environment to sub-processes (NEVER for production builds)",
           0, False)

add_option('variables-help',
           "Print the help text for SCons variables", 0, False)

if darwin:
    add_option("osx-version-min", "minimum OS X version to support", 1, True)

elif windows:
    win_version_min_choices = {
        'xpsp3'   : ('0501', '0300'),
        'ws03sp2' : ('0502', '0200'),
        'vista'   : ('0600', '0000'),
        'ws08r2'  : ('0601', '0000'),
        'win7'    : ('0601', '0000'),
        'win8'    : ('0602', '0000'),
    }

    add_option("win-version-min", "minimum Windows version to support", 1, True,
               type = 'choice', default = None,
               choices = win_version_min_choices.keys())

add_option('cache',
           "Use an object cache rather than a per-build variant directory (experimental)",
           0, False)

add_option('cache-dir',
           "Specify the directory to use for caching objects if --cache is in use",
           1, False, default="$BUILD_ROOT/scons/cache")

variable_parse_mode_choices=['auto', 'posix', 'other']
add_option('variable-parse-mode',
           "Select which parsing mode is used to interpret command line variables",
           1, False,
           type='choice', default=variable_parse_mode_choices[0],
           choices=variable_parse_mode_choices)

# Setup the command-line variables
def variable_shlex_converter(val):
    parse_mode = get_option('variable-parse-mode')
    if parse_mode == 'auto':
        parse_mode = 'other' if windows else 'posix'
    return shlex.split(val, posix=(parse_mode == 'posix'))

def variable_arch_converter(val):
    arches = {
        'x86_64': 'x86_64',
        'amd64':  'x86_64',
        'emt64':   'x86_64',
        'x86':    'i386',
    }
    val = val.lower()

    if val in arches:
        return arches[val]

    # Uname returns a bunch of possible x86's on Linux.
    # Check whether the value is an i[3456]86 processor.
    if re.match(r'^i[3-6]86$', val):
        return 'i386'

    # Return whatever val is passed in - hopefully it's legit
    return val

# The Scons 'default' tool enables a lot of tools that we don't actually need to enable.
# On platforms like Solaris, it actually does the wrong thing by enabling the sunstudio
# toolchain first. As such it is simpler and more efficient to manually load the precise
# set of tools we need for each platform.
# If we aren't on a platform where we know the minimal set of tools, we fall back to loading
# the 'default' tool.
def decide_platform_tools():
    if windows:
        # we only support MS toolchain on windows
        return ['msvc', 'mslink', 'mslib']
    elif linux:
        return ['gcc', 'g++', 'gnulink', 'ar']
    elif solaris:
        return ['gcc', 'g++', 'gnulink', 'ar']
    elif darwin:
        return ['gcc', 'g++', 'applelink', 'ar']
    else:
        return ["default"]

def variable_tools_converter(val):
    tool_list = shlex.split(val)
    return tool_list + ["jsheader", "mergelib", "mongo_unittest", "textfile"]

env_vars = Variables()

env_vars.Add('ARFLAGS',
    help='Sets flags for the archiver',
    converter=variable_shlex_converter)

env_vars.Add('CCFLAGS',
    help='Sets flags for the C and C++ compiler',
    converter=variable_shlex_converter)

env_vars.Add('CFLAGS',
    help='Sets flags for the C compiler',
    converter=variable_shlex_converter)

env_vars.Add('CPPDEFINES',
    help='Sets pre-processor definitions for C and C++',
    converter=variable_shlex_converter)

env_vars.Add('CPPPATH',
    help='Adds paths to the preprocessor search path',
    converter=variable_shlex_converter)

env_vars.Add('CXXFLAGS',
    help='Sets flags for the C++ compiler',
    converter=variable_shlex_converter)

env_vars.Add('HOST_ARCH',
    help='Sets the native architecture of the compiler',
    converter=variable_arch_converter,
    default=None)

env_vars.Add('LIBPATH',
    help='Adds paths to the linker search path',
    converter=variable_shlex_converter)

env_vars.Add('LIBS',
    help='Adds extra libraries to link against',
    converter=variable_shlex_converter)

env_vars.Add('LINKFLAGS',
    help='Sets flags for the linker',
    converter=variable_shlex_converter)

env_vars.Add('MSVC_USE_SCRIPT',
    help='Sets the script used to setup Visual Studio.')

env_vars.Add('MSVC_VERSION',
    help='Sets the version of Visual Studio to use (e.g.  12.0, 11.0, 10.0)')

env_vars.Add('RPATH',
    help='Set the RPATH for dynamic libraries and executables',
    converter=variable_shlex_converter)

env_vars.Add('SHCCFLAGS',
    help='Sets flags for the C and C++ compiler when building shared libraries',
    converter=variable_shlex_converter)

env_vars.Add('SHCFLAGS',
    help='Sets flags for the C compiler when building shared libraries',
    converter=variable_shlex_converter)

env_vars.Add('SHCXXFLAGS',
    help='Sets flags for the C++ compiler when building shared libraries',
    converter=variable_shlex_converter)

env_vars.Add('SHLINKFLAGS',
    help='Sets flags for the linker when building shared libraries',
    converter=variable_shlex_converter)

env_vars.Add('TARGET_ARCH',
    help='Sets the architecture to build for',
    converter=variable_arch_converter,
    default=None)

env_vars.Add('TOOLS',
    help='Sets the list of SCons tools to add to the environment',
    converter=variable_tools_converter,
    default=decide_platform_tools())

# don't run configure if user calls --help
if GetOption('help'):
    Return()

# --- environment setup ---

# If the user isn't using the # to indicate top-of-tree or $ to expand a variable, forbid
# relative paths. Relative paths don't really work as expected, because they end up relative to
# the top level SConstruct, not the invokers CWD. We could in theory fix this with
# GetLaunchDir, but that seems a step too far.
buildDir = get_option('build-dir').rstrip('/')
if buildDir[0] not in ['$', '#']:
    if not os.path.isabs(buildDir):
        print("Do not use relative paths with --build-dir")
        Exit(1)

cacheDir = get_option('cache-dir').rstrip('/')
if cacheDir[0] not in ['$', '#']:
    if not os.path.isabs(cacheDir):
        print("Do not use relative paths with --cache-dir")
        Exit(1)

installDir = get_option('prefix').rstrip('/')
if installDir[0] not in ['$', '#']:
    if not os.path.isabs(installDir):
        print("Do not use relative paths with --prefix")
        Exit(1)

sconsDataDir = Dir(buildDir).Dir('scons')
SConsignFile(str(sconsDataDir.File('sconsign')))

def printLocalInfo():
    import sys, SCons
    print( "scons version: " + SCons.__version__ )
    print( "python version: " + " ".join( [ `i` for i in sys.version_info ] ) )

printLocalInfo()

boostLibs = [ "thread" , "filesystem" , "program_options", "system" ]

onlyServer = len( COMMAND_LINE_TARGETS ) == 0 or ( len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) in [ "mongod" , "mongos" , "test" ] )

releaseBuild = has_option("release")

dbg_opt_mapping = {
    # --dbg, --opt   :   dbg    opt
    ( None,  None  ) : ( False, True ),
    ( None,  "on"  ) : ( False, True ),
    ( None,  "off" ) : ( False, False ),
    ( "on",  None  ) : ( True,  False ),  # special case interaction
    ( "on",  "on"  ) : ( True,  True ),
    ( "on",  "off" ) : ( True,  False ),
    ( "off", None  ) : ( False, True ),
    ( "off", "on"  ) : ( False, True ),
    ( "off", "off" ) : ( False, False ),
}
debugBuild, optBuild = dbg_opt_mapping[(get_option('dbg'), get_option('opt'))]

if releaseBuild and (debugBuild or not optBuild):
    print("Error: A --release build may not have debugging, and must have optimization")
    Exit(1)

noshell = has_option( "noshell" ) 

jsEngine = get_option( "js-engine")

serverJs = get_option( "server-js" ) == "on"

usev8 = (jsEngine != 'none')

v8version = jsEngine[3:] if jsEngine.startswith('v8-') else 'none'
v8suffix = '' if v8version == '3.12' else '-' + v8version

if not serverJs and not usev8:
    print("Warning: --server-js=off is not needed with --js-engine=none")

def getMongoCodeVersion():
    with open("version.txt") as version_txt:
        content = version_txt.readlines()
        if len(content) != 1:
            print("Malformed version file")
            Exit(1)
        return content[0].strip()

# We defer building the env until we have determined whether we want certain values. Some values
# in the env actually have semantics for 'None' that differ from being absent, so it is better
# to build it up via a dict, and then construct the Environment in one shot with kwargs.
#
# Yes, BUILD_ROOT vs BUILD_DIR is confusing. Ideally, BUILD_DIR would actually be called
# VARIANT_DIR, and at some point we should probably do that renaming. Until we do though, we
# also need an Environment variable for the argument to --build-dir, which is the parent of all
# variant dirs. For now, we call that BUILD_ROOT. If and when we s/BUILD_DIR/VARIANT_DIR/g,
# then also s/BUILD_ROOT/BUILD_DIR/g.
envDict = dict(BUILD_ROOT=buildDir,
               BUILD_DIR=get_variant_dir(),
               DIST_ARCHIVE_SUFFIX='.tgz',
               MODULE_BANNERS=[],
               ARCHIVE_ADDITION_DIR_MAP={},
               ARCHIVE_ADDITIONS=[],
               PYTHON=utils.find_python(),
               SERVER_ARCHIVE='${SERVER_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
               UNITTEST_ALIAS='unittests',
               # TODO: Move unittests.txt to $BUILD_DIR, but that requires
               # changes to MCI.
               UNITTEST_LIST='$BUILD_ROOT/unittests.txt',
               PYSYSPLATFORM=os.sys.platform,
               PCRE_VERSION='8.36',
               CONFIGUREDIR=sconsDataDir.Dir('sconf_temp'),
               CONFIGURELOG=sconsDataDir.File('config.log'),
               INSTALL_DIR=installDir,
               MONGO_GIT_VERSION=utils.getGitVersion(),
               MONGO_CODE_VERSION=getMongoCodeVersion(),
               )

env = Environment(variables=env_vars, **envDict)
del envDict

if has_option('variables-help'):
    print env_vars.GenerateHelpText(env)
    Exit(0)

unknown_vars = env_vars.UnknownVariables()
if unknown_vars:
    print "Unknown variables specified: {0}".format(", ".join(unknown_vars.keys()))
    Exit(1)

if has_option( "cc-use-shell-environment" ) and has_option( "cc" ):
    print("Cannot specify both --cc-use-shell-environment and --cc")
    Exit(1)
elif has_option( "cxx-use-shell-environment" ) and has_option( "cxx" ):
    print("Cannot specify both --cxx-use-shell-environment and --cxx")
    Exit(1)

if has_option( "cxx-use-shell-environment" ):
    env["CXX"] = os.getenv("CXX");
if has_option( "cc-use-shell-environment" ):
    env["CC"] = os.getenv("CC");

if has_option( "cxx" ):
    if not has_option( "cc" ):
        print "Must specify C compiler when specifying C++ compiler"
        Exit(1)
    env["CXX"] = get_option( "cxx" )
if has_option( "cc" ):
    if not has_option( "cxx" ):
        print "Must specify C++ compiler when specifying C compiler"
        Exit(1)
    env["CC"] = get_option( "cc" )

detectEnv = env.Clone()
# Identify the toolchain in use. We currently support the following:
# These macros came from
# http://nadeausoftware.com/articles/2012/10/c_c_tip_how_detect_compiler_name_and_version_using_compiler_predefined_macros
toolchain_macros = {
    'GCC': 'defined(__GNUC__) && !defined(__clang__)',
    'clang': 'defined(__clang__)',
    'MSVC': 'defined(_MSC_VER)'
}

def CheckForToolchain(context, toolchain, lang_name, compiler_var, source_suffix):
    test_body = textwrap.dedent("""
    #if {0}
    /* we are using toolchain {0} */
    #else
    #error
    #endif
    """.format(toolchain_macros[toolchain]))

    print_tuple = (lang_name, context.env[compiler_var], toolchain)
    context.Message('Checking if %s compiler "%s" is %s... ' % print_tuple)

    # Strip indentation from the test body to ensure that the newline at the end of the
    # endif is the last character in the file (rather than a line of spaces with no
    # newline), and that all of the preprocessor directives start at column zero. Both of
    # these issues can trip up older toolchains.
    result = context.TryCompile(test_body, source_suffix)
    context.Result(result)
    return result

# These preprocessor macros came from
# http://nadeausoftware.com/articles/2012/02/c_c_tip_how_detect_processor_type_using_compiler_predefined_macros
processor_macros = {
    'x86_64': ('__x86_64', '_M_AMD64'),
    'i386': ('__i386', '_M_IX86'),
    'sparc': ('__sparc'),
    'PowerPC': ('__powerpc__', '__PPC'),
    'arm' : ('__arm__'),
    'arm64' : ('__arm64__', '__aarch64__'),
}

def CheckForProcessor(context, which_arch):
    def run_compile_check(arch):
        full_macros = " || ".join([ "defined(%s)" % (v) for v in processor_macros[arch]])
        test_body = """
        #if {0}
        /* Detected {1} */
        #else
        #error not {1}
        #endif
        """.format(full_macros, arch)

        return context.TryCompile(textwrap.dedent(test_body), ".c")

    if which_arch:
        ret = run_compile_check(which_arch)
        context.Message('Checking if target processor is %s ' % which_arch)
        context.Result(ret)
        return ret;

    for k in processor_macros.keys():
        ret = run_compile_check(k)
        if ret:
            context.Result('Detected a %s processor' % k)
            return k

    context.Result('Could not detect processor model/architecture')
    return False

detectConf = Configure(detectEnv, help=False, custom_tests = {
    'CheckForToolchain' : CheckForToolchain,
    'CheckForProcessor': CheckForProcessor,
})

if not detectConf.CheckCXX():
    print "C++ compiler %s doesn't work" % (detectEnv['CXX'])
    Exit(1)
if not detectConf.CheckCC():
    print "C compiler %s doesn't work" % (detectEnv['CC'])
    Exit(1)

toolchain_search_sequence = [ "GCC", "clang" ]
detected_toolchain = None
if detectEnv['PYSYSPLATFORM'] == 'win32':
    toolchain_search_sequence = [ 'MSVC', 'clang', 'GCC' ]
for candidate_toolchain in toolchain_search_sequence:
    if detectConf.CheckForToolchain(candidate_toolchain, "C++", "CXX", ".cpp"):
        detected_toolchain = candidate_toolchain
        break

if not detected_toolchain:
    print "Couldn't identity the C++ compiler"
    Exit(1)

if not detectConf.CheckForToolchain(detected_toolchain, "C", "CC", ".c"):
    print "C compiler does not match identified C++ compiler"
    Exit(1)

# Now that we've detected the toolchain, we add methods to the env
# to get the canonical name of the toolchain and to test whether
# scons is using a particular toolchain.
def get_toolchain_name(self):
    return detected_toolchain.lower()
def is_toolchain(self, *args):
    actual_toolchain = self.ToolchainName()
    for v in args:
        if v.lower() == actual_toolchain:
            return True
    return False

env.AddMethod(get_toolchain_name, 'ToolchainName')
env.AddMethod(is_toolchain, 'ToolchainIs')

if env['TARGET_ARCH']:
    if not detectConf.CheckForProcessor(env['TARGET_ARCH']):
        print "Could not detect processor specified in TARGET_ARCH variable"
        Exit(1)
else:
    detected_processor = detectConf.CheckForProcessor(None)
    if not detected_processor:
        Exit(1)
    env['TARGET_ARCH'] = detected_processor

detectConf.Finish()

if not env['HOST_ARCH']:
    env['HOST_ARCH'] = env['TARGET_ARCH']

if has_option("cache"):
    if has_option("release"):
        print("Using the experimental --cache option is not permitted for --release builds")
        Exit(1)
    if has_option("gcov"):
        print("Mixing --cache and --gcov doesn't work correctly yet. See SERVER-11084")
        Exit(1)
    env.CacheDir(str(env.Dir(cacheDir)))

if optBuild:
    env.Append( CPPDEFINES=["MONGO_OPTIMIZED_BUILD"] )

if has_option("propagate-shell-environment"):
    env['ENV'] = dict(os.environ);

# Ignore requests to build fast and loose for release builds.
if get_option('build-fast-and-loose') == "on" and not has_option('release'):
    # See http://www.scons.org/wiki/GoFastButton for details
    env.Decider('MD5-timestamp')
    env.SetOption('max_drift', 1)

if has_option('mute'):
    env.Append( CCCOMSTR = "Compiling $TARGET" )
    env.Append( CXXCOMSTR = env["CCCOMSTR"] )
    env.Append( SHCCCOMSTR = "Compiling $TARGET" )
    env.Append( SHCXXCOMSTR = env["SHCCCOMSTR"] )
    env.Append( LINKCOMSTR = "Linking $TARGET" )
    env.Append( SHLINKCOMSTR = env["LINKCOMSTR"] )
    env.Append( ARCOMSTR = "Generating library $TARGET" )

endian = get_option( "endian" )

if endian == "auto":
    endian = sys.byteorder

if endian == "little":
    env.Append( CPPDEFINES=[("MONGO_BYTE_ORDER", "1234")] )
elif endian == "big":
    env.Append( CPPDEFINES=[("MONGO_BYTE_ORDER", "4321")] )

env['_LIBDEPS'] = '$_LIBDEPS_OBJS'

if env['_LIBDEPS'] == '$_LIBDEPS_OBJS':
    # The libraries we build in LIBDEPS_OBJS mode are just placeholders for tracking dependencies.
    # This avoids wasting time and disk IO on them.
    def write_uuid_to_file(env, target, source):
        with open(env.File(target[0]).abspath, 'w') as fake_lib:
            fake_lib.write(str(uuid.uuid4()))
            fake_lib.write('\n')

    def noop_action(env, target, source):
        pass

    env['ARCOM'] = write_uuid_to_file
    env['ARCOMSTR'] = 'Generating placeholder library $TARGET'
    env['RANLIBCOM'] = noop_action
    env['RANLIBCOMSTR'] = 'Skipping ranlib for $TARGET'

libdeps.setup_environment( env )

if env['PYSYSPLATFORM'] == 'linux3':
    env['PYSYSPLATFORM'] = 'linux2'
if 'freebsd' in env['PYSYSPLATFORM']:
    env['PYSYSPLATFORM'] = 'freebsd'

if os.sys.platform == 'win32':
    env['OS_FAMILY'] = 'win'
else:
    env['OS_FAMILY'] = 'posix'

if env['PYSYSPLATFORM'] in ('linux2', 'freebsd'):
    env['LINK_LIBGROUP_START'] = '-Wl,--start-group'
    env['LINK_LIBGROUP_END'] = '-Wl,--end-group'
elif env['PYSYSPLATFORM'] == 'darwin':
    env['LINK_LIBGROUP_START'] = ''
    env['LINK_LIBGROUP_END'] = ''
elif env['PYSYSPLATFORM'].startswith('sunos'):
    env['LINK_LIBGROUP_START'] = '-z rescan'
    env['LINK_LIBGROUP_END'] = ''

env.Prepend( CPPDEFINES=[ "MONGO_EXPOSE_MACROS" ,
                          "PCRE_STATIC",  # for pcre on Windows
                          "SUPPORT_UTF8" ],  # for pcre
)

if has_option( "safeshell" ):
    env.Append( CPPDEFINES=[ "MONGO_SAFE_SHELL" ] )

if has_option( "durableDefaultOn" ):
    env.Append( CPPDEFINES=[ "_DURABLEDEFAULTON" ] )

if has_option( "durableDefaultOff" ):
    env.Append( CPPDEFINES=[ "_DURABLEDEFAULTOFF" ] )

# ---- other build setup -----
dontReplacePackage = False
isBuildingLatest = False

def filterExists(paths):
    return filter(os.path.exists, paths)

if darwin:
    pass
elif linux:
    env.Append( LIBS=['m'] )

elif solaris:
     env.Append( LIBS=["socket","resolv","lgrp"] )

elif freebsd:
    env.Append( LIBS=[ "kvm" ] )
    env.Append( CCFLAGS=[ "-fno-omit-frame-pointer" ] )

elif openbsd:
    env.Append( LIBS=[ "kvm" ] )

elif windows:
    dynamicCRT = has_option("dynamic-windows")

    env['DIST_ARCHIVE_SUFFIX'] = '.zip'

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
    # c4290
    #  C++ exception specification ignored except to indicate a function is not __declspec(nothrow
    #  A function is declared using exception specification, which Visual C++ accepts but does not
    #  implement
    # c4068
    #  unknown pragma -- added so that we can specify unknown pragmas for other compilers
    # c4351
    #  on extremely old versions of MSVC (pre 2k5), default constructing an array member in a
    #  constructor's initialization list would not zero the array members "in some cases".
    #  since we don't target MSVC versions that old, this warning is safe to ignore.
    env.Append( CCFLAGS=["/wd4355", "/wd4800", "/wd4267", "/wd4244",
                         "/wd4290", "/wd4068", "/wd4351"] )

    # some warnings we should treat as errors:
    # c4099
    #  identifier' : type name first seen using 'objecttype1' now seen using 'objecttype2'
    #    This warning occurs when classes and structs are declared with a mix of struct and class
    #    which can cause linker failures
    # c4930
    #  'identifier': prototyped function not called (was a variable definition intended?)
    #     This warning indicates a most-vexing parse error, where a user declared a function that
    #     was probably intended as a variable definition.  A common example is accidentally
    #     declaring a function called lock that takes a mutex when one meant to create a guard
    #     object called lock on the stack.
    env.Append( CCFLAGS=["/we4099", "/we4930"] )

    env.Append( CPPDEFINES=["_CONSOLE","_CRT_SECURE_NO_WARNINGS"] )

    # this would be for pre-compiled headers, could play with it later  
    #env.Append( CCFLAGS=['/Yu"pch.h"'] )

    # docs say don't use /FD from command line (minimal rebuild)
    # /Gy function level linking (implicit when using /Z7)
    # /Z7 debug info goes into each individual .obj file -- no .pdb created 
    env.Append( CCFLAGS= ["/Z7", "/errorReport:none"] )

    # /DEBUG will tell the linker to create a .pdb file
    # which WinDbg and Visual Studio will use to resolve
    # symbols if you want to debug a release-mode image.
    # Note that this means we can't do parallel links in the build.
    #
    # Please also note that this has nothing to do with _DEBUG or optimization.
    env.Append( LINKFLAGS=["/DEBUG"] )

    # /MD:  use the multithreaded, DLL version of the run-time library (MSVCRT.lib/MSVCR###.DLL)
    # /MT:  use the multithreaded, static version of the run-time library (LIBCMT.lib)
    # /MDd: Defines _DEBUG, _MT, _DLL, and uses MSVCRTD.lib/MSVCRD###.DLL
    # /MTd: Defines _DEBUG, _MT, and causes your application to use the
    #       debug multithread version of the run-time library (LIBCMTD.lib)

    winRuntimeLibMap = {
          #dyn   #dbg
        ( False, False ) : "/MT",
        ( False, True  ) : "/MTd",
        ( True,  False ) : "/MD",
        ( True,  True  ) : "/MDd",
    }

    env.Append(CCFLAGS=[winRuntimeLibMap[(dynamicCRT, debugBuild)]])

    # With VS 2012 and later we need to specify 5.01 as the target console
    # so that our 32-bit builds run on Windows XP
    # See https://software.intel.com/en-us/articles/linking-applications-using-visual-studio-2012-to-run-on-windows-xp
    #
    if env["TARGET_ARCH"] == "i386":
        env.Append( LINKFLAGS=["/SUBSYSTEM:CONSOLE,5.01"])

    if optBuild:
        # /O2:  optimize for speed (as opposed to size)
        # /Oy-: disable frame pointer optimization (overrides /O2, only affects 32-bit)
        # /INCREMENTAL: NO - disable incremental link - avoid the level of indirection for function
        # calls
        env.Append( CCFLAGS=["/O2", "/Oy-"] )
        env.Append( LINKFLAGS=["/INCREMENTAL:NO"])
    else:
        env.Append( CCFLAGS=["/Od"] )

    if debugBuild and not optBuild:
        # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used
        # without having been initialized (implies /Od: no optimizations)
        env.Append( CCFLAGS=["/RTC1"] )

    # This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds
    env.Append( LINKFLAGS=["/LARGEADDRESSAWARE"] )

    env.Append(LIBS=['ws2_32.lib',
                     'kernel32.lib',
                     'advapi32.lib',
                     'Psapi.lib',
                     'DbgHelp.lib',
                     'shell32.lib',
                     'Iphlpapi.lib',
                     'version.lib'])

    # v8 calls timeGetTime()
    if usev8:
        env.Append(LIBS=['winmm.lib'])

env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1
if nix:

    if has_option( "static-libstdc++" ):
        env.Append( LINKFLAGS=["-static-libstdc++", "-static-libgcc"] )

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
        env.Append( CCFLAGS=["-pipe"] )
        if not has_option("disable-warnings-as-errors"):
            env.Append( CCFLAGS=["-Werror"] )

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
        env.Append( CPPDEFINES=["MONGO_GCOV"] )
        env.Append( LINKFLAGS=" -fprofile-arcs -ftest-coverage " )

    if optBuild:
        env.Append( CCFLAGS=["-O3"] )
    else:
        env.Append( CCFLAGS=["-O0"] )

    if debugBuild:
        if not optBuild:
            env.Append( CCFLAGS=["-fstack-protector"] )
            env.Append( LINKFLAGS=["-fstack-protector"] )
            env.Append( SHLINKFLAGS=["-fstack-protector"] )
        env['ENV']['GLIBCXX_FORCE_NEW'] = 1; # play nice with valgrind
        env.Append( CPPDEFINES=["_DEBUG"] );

if has_option( "ssl" ):
    env.Append( CPPDEFINES=["MONGO_SSL"] )
    env.Append( MONGO_CRYPTO=["openssl"] )
    if windows:
        env.Append( LIBS=["libeay32"] )
        env.Append( LIBS=["ssleay32"] )
    else:
        env.Append( LIBS=["ssl"] )
        env.Append( LIBS=["crypto"] )
    if has_option("ssl-fips-capability"):
        env.Append( CPPDEFINES=["MONGO_SSL_FIPS"] )
else:
    env.Append( MONGO_CRYPTO=["tom"] )

wiredtiger = False
if get_option('wiredtiger') == 'on':
    # Wiredtiger only supports 64-bit architecture, and will fail to compile on 32-bit
    # so disable WiredTiger automatically on 32-bit since wiredtiger is on by default
    if env['TARGET_ARCH'] == 'i386':
        print "WiredTiger is not supported on 32-bit platforms"
        print "Re-run scons with --wiredtiger=off to build on 32-bit platforms"
        Exit(1)
    else:
        wiredtiger = True

if env['TARGET_ARCH'] == 'i386':
    # If we are using GCC or clang to target 32 bit, set the ISA minimum to 'nocona',
    # and the tuning to 'generic'. The choice of 'nocona' is selected because it
    #  -- includes MMX extenions which we need for tcmalloc on 32-bit
    #  -- can target 32 bit
    #  -- is at the time of this writing a widely-deployed 10 year old microarchitecture
    #  -- is available as a target architecture from GCC 4.0+
    # However, we only want to select an ISA, not the nocona specific scheduling, so we
    # select the generic tuning. For installations where hardware and system compiler rev are
    # contemporaries, the generic scheduling should be appropriate for a wide range of
    # deployed hardware.

    if env.ToolchainIs('GCC', 'clang'):
        env.Append( CCFLAGS=['-march=nocona', '-mtune=generic'] )

try:
    umask = os.umask(022)
except OSError:
    pass

if not windows:
    for keysuffix in [ "1" , "2" ]:
        keyfile = "jstests/libs/key%s" % keysuffix
        os.chmod( keyfile , stat.S_IWUSR|stat.S_IRUSR )

# boostSuffixList is used when using system boost to select a search sequence
# for boost libraries.
boostSuffixList = ["-mt", ""]
if get_option("system-boost-lib-search-suffixes") is not None:
    if not use_system_version_of_library("boost"):
        print("The --system-boost-lib-search-suffixes option is only valid with --use-system-boost")
        Exit(1)
    boostSuffixList = get_option("system-boost-lib-search-suffixes")
    if boostSuffixList == "":
        boostSuffixList = []
    else:
        boostSuffixList = boostSuffixList.split(',')

# boostSuffix is used when using internal boost to select which version
# of boost is in play.
boostSuffix = "";
if not use_system_version_of_library("boost"):
    # Boost release numbers are x.y.z, where z is usually 0 which we do not include in
    # the internal-boost option
    boostSuffix = "-%s.0" % get_option( "internal-boost")
    env.Prepend(CPPDEFINES=['BOOST_ALL_NO_LIB'])

# discover modules, and load the (python) module for each module's build.py
mongo_modules = moduleconfig.discover_modules('src/mongo/db/modules')
env['MONGO_MODULES'] = [m.name for m in mongo_modules]

# --- check system ---

def doConfigure(myenv):
    global wiredtiger

    # Check that the compilers work.
    #
    # TODO: Currently, we have some flags already injected. Eventually, this should test the
    # bare compilers, and we should re-check at the very end that TryCompile and TryLink still
    # work with the flags we have selected.
    if myenv.ToolchainIs('msvc'):
        compiler_minimum_string = "Microsoft Visual Studio 2013 Update 2"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(_MSC_VER)
        #error
        #endif

        #if _MSC_VER < 1800 || (_MSC_VER == 1800 && _MSC_FULL_VER < 180030501)
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('gcc'):
        compiler_minimum_string = "GCC 4.8.2"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(__GNUC__) || defined(__clang__)
        #error
        #endif

        #if (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ < 8) || (__GNUC__ == 4 && __GNUC_MINOR__ == 8 && __GNUC_PATCHLEVEL__ < 2)
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('clang'):
        compiler_minimum_string = "clang 3.4 (or Apple XCode 5.1.1)"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(__clang__)
        #error
        #endif

        #if defined(__apple_build_version__)
        #if __apple_build_version__ < 5030040
        #error %s or newer is required to build MongoDB
        #endif
        #elif (__clang_major__ < 3) || (__clang_major__ == 3 && __clang_minor__ < 4)
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % (compiler_minimum_string, compiler_minimum_string))
    else:
        print("Error: can't check compiler minimum; don't know this compiler...")
        Exit(1)

    def CheckForMinimumCompiler(context, language):
        extension_for = {
            "C" : ".c",
            "C++" : ".cpp",
        }
        context.Message("Checking if %s compiler is %s or newer..." %
                        (language, compiler_minimum_string))
        result = context.TryCompile(compiler_test_body, extension_for[language])
        context.Result(result)
        return result;

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckForMinimumCompiler' : CheckForMinimumCompiler,
    })

    c_compiler_validated = conf.CheckForMinimumCompiler('C')
    cxx_compiler_validated = conf.CheckForMinimumCompiler('C++')

    suppress_invalid = has_option("disable-minimum-compiler-version-enforcement")
    if releaseBuild and suppress_invalid:
        print("--disable-minimum-compiler-version-enforcement is forbidden with --release")
        Exit(1)

    if not (c_compiler_validated and cxx_compiler_validated):
        if not suppress_invalid:
            print("ERROR: Refusing to build with compiler that does not meet requirements")
            Exit(1)
        print("WARNING: Ignoring failed compiler version check per explicit user request.")
        print("WARNING: The build may fail, binaries may crash, or may run but corrupt data...")

    # Figure out what our minimum windows version is. If the user has specified, then use
    # that. Otherwise, if they have explicitly selected between 32 bit or 64 bit, choose XP or
    # Vista respectively. Finally, if they haven't done either of these, try invoking the
    # compiler to figure out whether we are doing a 32 or 64 bit build and select as
    # appropriate.
    if windows:
        win_version_min = None
        if has_option('win-version-min'):
            win_version_min = get_option('win-version-min')
        # If no minimum version has beeen specified, use our defaults for 32-bit/64-bit windows.
        elif env['TARGET_ARCH'] == 'x86_64':
            win_version_min = 'ws03sp2'
        elif env['TARGET_ARCH'] == 'i386':
            win_version_min = 'xpsp3'

        env['WIN_VERSION_MIN'] = win_version_min
        win_version_min = win_version_min_choices[win_version_min]
        env.Append( CPPDEFINES=[("_WIN32_WINNT", "0x" + win_version_min[0])] )
        env.Append( CPPDEFINES=[("NTDDI_VERSION", "0x" + win_version_min[0] + win_version_min[1])] )

    conf.Finish()

    def AddFlagIfSupported(env, tool, extension, flag, **mutation):
        def CheckFlagTest(context, tool, extension, flag):
            test_body = ""
            context.Message('Checking if %s compiler supports %s... ' % (tool, flag))
            ret = context.TryCompile(test_body, extension)
            context.Result(ret)
            return ret

        if env.ToolchainIs('msvc'):
            print("AddFlagIfSupported is not currently supported with MSVC")
            Exit(1)

        test_mutation = mutation
        if env.ToolchainIs('gcc'):
            test_mutation = copy.deepcopy(mutation)
            # GCC helpfully doesn't issue a diagnostic on unknown flags of the form -Wno-xxx
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

    if myenv.ToolchainIs('clang', 'gcc'):
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

        # Prevents warning about using deprecated features (such as auto_ptr in c++11)
        # Using -Wno-error=deprecated-declarations does not seem to work on some compilers,
        # including at least g++-4.6.
        AddToCCFLAGSIfSupported(myenv, "-Wno-deprecated-declarations")

        # As of clang-3.4, this warning appears in v8, and gets escalated to an error.
        AddToCCFLAGSIfSupported(myenv, "-Wno-tautological-constant-out-of-range-compare")

        # New in clang-3.4, trips up things mostly in third_party, but in a few places in the
        # primary mongo sources as well.
        AddToCCFLAGSIfSupported(myenv, "-Wno-unused-const-variable")

        # Prevents warning about unused but set variables found in boost version 1.49
        # in boost/date_time/format_date_parser.hpp which does not work for compilers
        # GCC >= 4.6. Error explained in https://svn.boost.org/trac/boost/ticket/6136 .
        AddToCCFLAGSIfSupported(myenv, "-Wno-unused-but-set-variable")

        # This has been suppressed in gcc 4.8, due to false positives, but not in clang.  So
        # we explicitly disable it here.
        AddToCCFLAGSIfSupported(myenv, "-Wno-missing-braces")

    # Check if we need to disable null-conversion warnings
    if myenv.ToolchainIs('clang'):
        def CheckNullConversion(context):

            test_body = """
            #include <boost/shared_ptr.hpp>
            struct TestType { int value; bool boolValue; };
            bool foo() {
                boost::shared_ptr<TestType> sp(new TestType);
                return NULL != sp;
            }
            """

            context.Message('Checking if implicit boost::shared_ptr null conversion is supported... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckNullConversion' : CheckNullConversion,
        })
        if conf.CheckNullConversion() == False:
            env.Append( CCFLAGS="-Wno-null-conversion" )
        conf.Finish()

    # This needs to happen before we check for libc++, since it affects whether libc++ is available.
    if darwin and has_option('osx-version-min'):
        min_version = get_option('osx-version-min')
        min_version_flag = '-mmacosx-version-min=%s' % (min_version)
        if not AddToCCFLAGSIfSupported(myenv, min_version_flag):
            print( "Can't set minimum OS X version with this compiler" )
            Exit(1)
        myenv.AppendUnique(LINKFLAGS=[min_version_flag])

    usingLibStdCxx = False
    if has_option('libc++'):
        if not myenv.ToolchainIs('clang'):
            print( 'libc++ is currently only supported for clang')
            Exit(1)
        if darwin and has_option('osx-version-min') and versiontuple(min_version) < versiontuple('10.7'):
            print("Warning: You passed option 'libc++'. You probably want to also pass 'osx-version-min=10.7' or higher for libc++ support.")
        if AddToCXXFLAGSIfSupported(myenv, '-stdlib=libc++'):
            myenv.Append(LINKFLAGS=['-stdlib=libc++'])
        else:
            print( 'libc++ requested, but compiler does not support -stdlib=libc++' )
            Exit(1)
    else:
        def CheckLibStdCxx(context):
            test_body = """
            #include <vector>
            #if !defined(__GLIBCXX__)
            #error
            #endif
            """

            context.Message('Checking if we are using libstdc++... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckLibStdCxx' : CheckLibStdCxx,
        })
        usingLibStdCxx = conf.CheckLibStdCxx()
        conf.Finish()

    if not myenv.ToolchainIs('msvc'):
        if not AddToCXXFLAGSIfSupported(myenv, '-std=c++11'):
            print( 'Compiler does not honor -std=c++11' )
            Exit(1)
        if not AddToCFLAGSIfSupported(myenv, '-std=c99'):
            print( "C++11 mode selected for C++ files, but can't enable C99 for C files" )
            Exit(1)

    if using_system_version_of_cxx_libraries():
        print( 'WARNING: System versions of C++ libraries must be compiled with C++11 support' )

    # We appear to have C++11, or at least a flag to enable it. Check that the declared C++
    # language level is not less than C++11, and that we can at least compile an 'auto'
    # expression. We don't check the __cplusplus macro when using MSVC because as of our
    # current required MS compiler version (MSVS 2013 Update 2), they don't set it. If
    # MSFT ever decides (in MSVS 2015?) to define __cplusplus >= 201103L, remove the exception
    # here for _MSC_VER
    def CheckCxx11(context):
        test_body = """
        #ifndef _MSC_VER
        #if __cplusplus < 201103L
        #error
        #endif
        #endif
        auto not_an_empty_file = 0;
        """

        context.Message('Checking for C++11... ')
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCxx11' : CheckCxx11,
    })

    if not conf.CheckCxx11():
        print( 'C++11 support is required to build MongoDB')
        Exit(1)

    conf.Finish()

    # If we are using libstdc++, check to see if we are using a libstdc++ that is older than
    # our GCC minimum of 4.8.2. This is primarly to help people using clang on OS X but
    # forgetting to use --libc++ (or set the target OS X version high enough to get it as the
    # default). We would, ideally, check the __GLIBCXX__ version, but for various reasons this
    # is not workable. Instead, we switch on the fact that _GLIBCXX_PROFILE_UNORDERED wasn't
    # introduced until libstdc++ 4.8.2. Yes, this is a terrible hack.
    if usingLibStdCxx:
        def CheckModernLibStdCxx(context):
            test_body = """
            #define _GLIBCXX_PROFILE
            #include <unordered_map>
            #if !defined(_GLIBCXX_PROFILE_UNORDERED)
            #error libstdc++ older than 4.8.2
            #endif
            """

            context.Message('Checking for libstdc++ 4.8.2 or better... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckModernLibStdCxx' : CheckModernLibStdCxx,
        })

        if not conf.CheckModernLibStdCxx():
            print("When using libstdc++, MongoDB requires libstdc++ 4.8.2 or newer")
            Exit(1)

        conf.Finish()

    if has_option("use-glibcxx-debug"):
        # If we are using a modern libstdc++ and this is a debug build and we control all C++
        # dependencies, then turn on the debugging features in libstdc++.
        # TODO: Need a new check here.
        if not debugBuild:
            print("--use-glibcxx-debug requires --dbg=on")
            Exit(1)
        if not usingLibStdCxx:
            print("--use-glibcxx-debug is only compatible with the GNU implementation of the "
                  "C++ standard libary")
            Exit(1)
        if using_system_version_of_cxx_libraries():
            print("--use-glibcxx-debug not compatible with system versions of C++ libraries.")
            Exit(1)
        myenv.Append(CPPDEFINES=["_GLIBCXX_DEBUG"]);

    # Check if we are on a POSIX system by testing if _POSIX_VERSION is defined.
    def CheckPosixSystem(context):

        test_body = """
        // POSIX requires the existence of unistd.h, so if we can't include unistd.h, we
        // are definitely not a POSIX system.
        #include <unistd.h>
        #if !defined(_POSIX_VERSION)
        #error not a POSIX system
        #endif
        """

        context.Message('Checking if we are on a POSIX system... ')
        ret = context.TryCompile(textwrap.dedent(test_body), ".c")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckPosixSystem' : CheckPosixSystem,
    })
    posix_system = conf.CheckPosixSystem()
    conf.Finish()

    # Check if we are on a system that support the POSIX clock_gettime function
    #  and the "monotonic" clock.
    posix_monotonic_clock = False
    if posix_system:
        def CheckPosixMonotonicClock(context):

            test_body = """
            #include <unistd.h>
            #if !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0)
            #error POSIX clock_gettime not supported
            #elif !(defined(_POSIX_MONOTONIC_CLOCK) && _POSIX_MONOTONIC_CLOCK >= 0)
            #error POSIX monotonic clock not supported
            #endif
            """

            context.Message('Checking if the POSIX monotonic clock is supported... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckPosixMonotonicClock' : CheckPosixMonotonicClock,
        })
        posix_monotonic_clock = conf.CheckPosixMonotonicClock()
        conf.Finish()

    if has_option('sanitize'):

        if not myenv.ToolchainIs('clang', 'gcc'):
            print( 'sanitize is only supported with clang or gcc')
            Exit(1)

        if get_option('allocator') == 'tcmalloc':
            # There are multiply defined symbols between the sanitizer and
            # our vendorized tcmalloc.
            print("Cannot use --sanitize with tcmalloc")
            Exit(1)

        sanitizer_list = get_option('sanitize').split(',')

        using_lsan = 'leak' in sanitizer_list
        using_asan = 'address' in sanitizer_list or using_lsan
        using_tsan = 'thread' in sanitizer_list

        # If the user asked for leak sanitizer, turn on the detect_leaks
        # ASAN_OPTION. If they asked for address sanitizer as well, drop
        # 'leak', because -fsanitize=leak means no address.
        #
        # --sanitize=leak:           -fsanitize=leak, detect_leaks=1
        # --sanitize=address,leak:   -fsanitize=address, detect_leaks=1
        # --sanitize=address:        -fsanitize=address
        #
        if using_lsan:
            if using_asan:
                myenv['ENV']['ASAN_OPTIONS'] = "detect_leaks=1"
            myenv['ENV']['LSAN_OPTIONS'] = "suppressions=%s" % myenv.File("#etc/lsan.suppressions").abspath
            if 'address' in sanitizer_list:
                sanitizer_list.remove('leak')

        sanitizer_option = '-fsanitize=' + ','.join(sanitizer_list)

        if AddToCCFLAGSIfSupported(myenv, sanitizer_option):
            myenv.Append(LINKFLAGS=[sanitizer_option])
            myenv.Append(CCFLAGS=['-fno-omit-frame-pointer'])
        else:
            print( 'Failed to enable sanitizers with flag: ' + sanitizer_option )
            Exit(1)

        blackfiles_map = {
            "address" : myenv.File("#etc/asan.blacklist"),
            "leak" : myenv.File("#etc/asan.blacklist"),
            "thread" : myenv.File("#etc/tsan.blacklist"),
            "undefined" : myenv.File("#etc/ubsan.blacklist"),
        }

        blackfiles = set([v for (k, v) in blackfiles_map.iteritems() if k in sanitizer_list])
        blacklist_options=["-fsanitize-blacklist=%s" % blackfile for blackfile in blackfiles]

        for blacklist_option in blacklist_options:
            if AddToCCFLAGSIfSupported(myenv, blacklist_option):
                myenv.Append(LINKFLAGS=[blacklist_option])

        llvm_symbolizer = get_option('llvm-symbolizer')
        if os.path.isabs(llvm_symbolizer):
            if not myenv.File(llvm_symbolizer).exists():
                print("WARNING: Specified symbolizer '%s' not found" % llvm_symbolizer)
                llvm_symbolizer = None
        else:
            llvm_symbolizer = myenv.WhereIs(llvm_symbolizer)

        if llvm_symbolizer:
            myenv['ENV']['ASAN_SYMBOLIZER_PATH'] = llvm_symbolizer
            myenv['ENV']['LSAN_SYMBOLIZER_PATH'] = llvm_symbolizer
            tsan_options = "external_symbolizer_path=\"%s\" " % llvm_symbolizer
        elif using_lsan:
            print("Using the leak sanitizer requires a valid symbolizer")
            Exit(1)

        if using_tsan:
            tsan_options += "suppressions=\"%s\" " % myenv.File("#etc/tsan.suppressions").abspath
            myenv['ENV']['TSAN_OPTIONS'] = tsan_options

    if myenv.ToolchainIs('msvc') and optBuild:
        # http://blogs.msdn.com/b/vcblog/archive/2013/09/11/introducing-gw-compiler-switch.aspx
        #
        myenv.Append( CCFLAGS=["/Gw", "/Gy"] )
        myenv.Append( LINKFLAGS=["/OPT:REF"])

        # http://blogs.msdn.com/b/vcblog/archive/2014/03/25/linker-enhancements-in-visual-studio-2013-update-2-ctp2.aspx
        #
        myenv.Append( CCFLAGS=["/Zc:inline"])

    # Apply any link time optimization settings as selected by the 'lto' option.
    if has_option('lto'):
        if myenv.ToolchainIs('msvc'):
            # Note that this is actually more aggressive than LTO, it is whole program
            # optimization due to /GL. However, this is historically what we have done for
            # windows, so we are keeping it.
            #
            # /GL implies /LTCG, so no need to say it in CCFLAGS, but we do need /LTCG on the
            # link flags.
            myenv.Append(CCFLAGS=['/GL'])
            myenv.Append(LINKFLAGS=['/LTCG'])
            myenv.Append(ARFLAGS=['/LTCG'])
        elif myenv.ToolchainIs('gcc', 'clang'):
            # For GCC and clang, the flag is -flto, and we need to pass it both on the compile
            # and link lines.
            if AddToCCFLAGSIfSupported(myenv, '-flto'):
                myenv.Append(LINKFLAGS=['-flto'])

                def LinkHelloWorld(context, adornment = None):
                    test_body = """
                    #include <iostream>
                    int main() {
                        std::cout << "Hello, World!" << std::endl;
                        return 0;
                    }
                    """
                    message = "Trying to link with LTO"
                    if adornment:
                        message = message + " " + adornment
                    message = message + "..."
                    context.Message(message)
                    ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
                    context.Result(ret)
                    return ret

                conf = Configure(myenv, help=False, custom_tests = {
                    'LinkHelloWorld' : LinkHelloWorld,
                })

                # Some systems (clang, on a system with the BFD linker by default) may need to
                # explicitly request the gold linker for LTO to work. If we can't LTO link a
                # simple program, see if -fuse=ld=gold helps.
                if not conf.LinkHelloWorld():
                    conf.env.Append(LINKFLAGS=["-fuse-ld=gold"])
                    if not conf.LinkHelloWorld("(with -fuse-ld=gold)"):
                        print("Error: Couldn't link with LTO")
                        Exit(1)

                myenv = conf.Finish()

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

    # When using msvc, check for support for __declspec(thread), unless we have been asked
    # explicitly not to use it. For other compilers, see if __thread works.
    if myenv.ToolchainIs('msvc'):
        haveDeclSpecThread = False
        def CheckDeclspecThread(context):
            test_body = """
            __declspec( thread ) int tsp_int;
            int main(int argc, char* argv[]) {
            tsp_int = argc;
            return 0;
            }
            """
            context.Message('Checking for __declspec(thread)... ')
            ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret
        conf = Configure(myenv, help=False, custom_tests = {
            'CheckDeclspecThread' : CheckDeclspecThread,
        })
        haveDeclSpecThread = conf.CheckDeclspecThread()
        conf.Finish()
        if haveDeclSpecThread:
            myenv.Append(CPPDEFINES=['MONGO_HAVE___DECLSPEC_THREAD'])
    else:
        def CheckUUThread(context):
            test_body = """
            __thread int tsp_int;
            int main(int argc, char* argv[]) {
                tsp_int = argc;
                return 0;
            }
            """
            context.Message('Checking for __thread... ')
            ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret
        conf = Configure(myenv, help=False, custom_tests = {
            'CheckUUThread' : CheckUUThread,
        })
        haveUUThread = conf.CheckUUThread()
        conf.Finish()
        if haveUUThread:
            myenv.Append(CPPDEFINES=['MONGO_HAVE___THREAD'])

    # not all C++11-enabled gcc versions have type properties
    def CheckCXX11IsTriviallyCopyable(context):
        test_body = """
        #include <type_traits>
        int main(int argc, char **argv) {
            class Trivial {
                int trivial1;
                double trivial2;
                struct {
                    float trivial3;
                    short trivial4;
                } trivial_member;
            };

            class NotTrivial {
                int x, y;
                NotTrivial(const NotTrivial& o) : x(o.y), y(o.x) {}
            };

            static_assert(std::is_trivially_copyable<Trivial>::value,
                          "I should be trivially copyable");
            static_assert(!std::is_trivially_copyable<NotTrivial>::value,
                          "I should not be trivially copyable");
            return 0;
        }
        """
        context.Message('Checking for C++11 is_trivially_copyable support... ')
        ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
        context.Result(ret)
        return ret

    # Some GCC's don't have std::is_trivially_copyable
    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCXX11IsTriviallyCopyable': CheckCXX11IsTriviallyCopyable,
    })

    if conf.CheckCXX11IsTriviallyCopyable():
        conf.env.Append(CPPDEFINES=['MONGO_HAVE_STD_IS_TRIVIALLY_COPYABLE'])

    myenv = conf.Finish()

    def CheckCXX14MakeUnique(context):
        test_body = """
        #include <memory>
        int main(int argc, char **argv) {
            auto foo = std::make_unique<int>(5);
            return 0;
        }
        """
        context.Message('Checking for C++14 std::make_unique support... ')
        ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
        context.Result(ret)
        return ret

    # Check for std::make_unique support without using the __cplusplus macro
    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCXX14MakeUnique': CheckCXX14MakeUnique,
    })

    if conf.CheckCXX14MakeUnique():
        conf.env.Append(CPPDEFINES=['MONGO_HAVE_STD_MAKE_UNIQUE'])

    myenv = conf.Finish()

    def CheckBoostMinVersion(context):
        compile_test_body = textwrap.dedent("""
        #include <boost/version.hpp>

        #if BOOST_VERSION < 104900
        #error
        #endif
        """)

        context.Message("Checking if system boost version is 1.49 or newer...")
        result = context.TryCompile(compile_test_body, ".cpp")
        context.Result(result)
        return result

    conf = Configure(myenv, custom_tests = {
        'CheckBoostMinVersion': CheckBoostMinVersion,
    })
    libdeps.setup_conftests(conf)

    if use_system_version_of_library("pcre"):
        conf.FindSysLibDep("pcre", ["pcre"])
        conf.FindSysLibDep("pcrecpp", ["pcrecpp"])

    if use_system_version_of_library("snappy"):
        conf.FindSysLibDep("snappy", ["snappy"])

    if use_system_version_of_library("zlib"):
        conf.FindSysLibDep("zlib", ["zlib" if windows else "z"])

    if use_system_version_of_library("stemmer"):
        conf.FindSysLibDep("stemmer", ["stemmer"])

    if use_system_version_of_library("yaml"):
        conf.FindSysLibDep("yaml", ["yaml-cpp"])

    if wiredtiger and use_system_version_of_library("wiredtiger"):
        if not conf.CheckCXXHeader( "wiredtiger.h" ):
            print( "Cannot find wiredtiger headers" )
            Exit(1)
        conf.FindSysLibDep("wiredtiger", ["wiredtiger"])

    if use_system_version_of_library("boost"):
        if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
            print( "can't find boost headers" )
            Exit(1)
        if not conf.CheckBoostMinVersion():
            print( "system's version of boost is too old. version 1.49 or better required")
            Exit(1)

        conf.env.Append(CPPDEFINES=[("BOOST_THREAD_VERSION", "2")])

        # Note that on Windows with using-system-boost builds, the following
        # FindSysLibDep calls do nothing useful (but nothing problematic either)
        #
        # NOTE: Pass --system-boost-lib-search-suffixes= to suppress these checks, which you
        # might want to do if using autolib linking on Windows, for example.
        if boostSuffixList:
            for b in boostLibs:
                boostlib = "boost_" + b
                conf.FindSysLibDep(
                    boostlib,
                    [boostlib + suffix for suffix in boostSuffixList],
                    language='C++')

    if posix_system:
        conf.env.Append(CPPDEFINES=['MONGO_HAVE_HEADER_UNISTD_H'])
        conf.CheckLib('rt')
        conf.CheckLib('dl')

    if posix_monotonic_clock:
        conf.env.Append(CPPDEFINES=['MONGO_HAVE_POSIX_MONOTONIC_CLOCK'])

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
            "sasl2", 
            ["stddef.h","sasl/sasl.h"], 
            "C", 
            "sasl_version_info(0, 0, 0, 0, 0, 0);", 
            autoadd=False ):
        Exit(1)

    # requires ports devel/libexecinfo to be installed
    if freebsd or openbsd:
        if not conf.CheckLib("execinfo"):
            print("Cannot find libexecinfo, please install devel/libexecinfo.")
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
        if not debugBuild:
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

def checkErrorCodes():
    import buildscripts.errorcodes as x
    if x.checkErrorCodes() == False:
        print( "next id to use:" + str( x.getNextCode() ) )
        Exit(-1)

checkErrorCodes()

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
    dist_arch = GetOption("distarch")
    arch_name = env['TARGET_ARCH'] if not dist_arch else dist_arch
    n = platform + "-" + arch_name
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

mongoCodeVersion = env['MONGO_CODE_VERSION']
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
Export("get_option")
Export("has_option use_system_version_of_library")
Export("serverJs")
Export("usev8")
Export("v8version v8suffix")
Export("boostSuffix")
Export("darwin windows solaris linux freebsd nix openbsd")
Export('module_sconscripts')
Export("debugBuild optBuild")
Export("s3push")
Export("wiredtiger")

def injectMongoIncludePaths(thisEnv):
    thisEnv.AppendUnique(CPPPATH=['$BUILD_DIR'])
env.AddMethod(injectMongoIncludePaths, 'InjectMongoIncludePaths')

env.SConscript('src/SConscript', variant_dir='$BUILD_DIR', duplicate=False)
env.SConscript('SConscript.smoke')

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

env.Alias('all', ['core', 'tools', 'dbtest', 'unittests', 'file_allocator_bench'])
