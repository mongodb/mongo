# -*- mode: python; -*-
import atexit
import copy
import datetime
import errno
import json
import os
import re
import shlex
import shutil
import stat
import sys
import textwrap
import uuid
from buildscripts import utils
from buildscripts import moduleconfig

from mongo_scons_utils import (
    default_buildinfo_environment_data,
    default_variant_dir_generator,
    get_toolchain_ver,
)

import libdeps

EnsureSConsVersion( 2, 3, 0 )

def print_build_failures():
    from SCons.Script import GetBuildFailures
    for bf in GetBuildFailures():
        print "%s failed: %s" % (bf.node, bf.errstr)
atexit.register(print_build_failures)

def versiontuple(v):
    return tuple(map(int, (v.split("."))))

# --- OS identification ---
#
# This needs to precede the options section so that we can only offer some options on certain
# operating systems.

# This function gets the running OS as identified by Python
# It should only be used to set up defaults for options/variables, because
# its value could potentially be overridden by setting TARGET_OS on the
# command-line. Treat this output as the value of HOST_OS
def get_running_os_name():
    running_os = os.sys.platform
    if running_os.startswith('linux'):
        running_os = 'linux'
    elif running_os.startswith('freebsd'):
        running_os = 'freebsd'
    elif running_os.startswith('openbsd'):
        running_os = 'openbsd'
    elif running_os == 'sunos5':
        running_os = 'solaris'
    elif running_os == 'win32':
        running_os = 'windows'
    elif running_os == 'darwin':
        running_os = 'osx'
    else:
        running_os = 'unknown'
    return running_os

def env_get_os_name_wrapper(self):
    return env['TARGET_OS']

def is_os_raw(target_os, os_list_to_check):
    okay = False
    posix_os_list = [ 'linux', 'openbsd', 'freebsd', 'osx', 'solaris' ]

    for p in os_list_to_check:
        if p == 'posix' and target_os in posix_os_list:
            okay = True
            break
        elif p == target_os:
            okay = True
            break
    return okay

# This function tests the running OS as identified by Python
# It should only be used to set up defaults for options/variables, because
# its value could potentially be overridden by setting TARGET_OS on the
# command-line. Treat this output as the value of HOST_OS
def is_running_os(*os_list):
    return is_os_raw(get_running_os_name(), os_list)

def env_os_is_wrapper(self, *os_list):
    return is_os_raw(self['TARGET_OS'], os_list)

def add_option(name, **kwargs):

    if 'dest' not in kwargs:
        kwargs['dest'] = name

    if 'metavar' not in kwargs and kwargs.get('type', None) == 'choice':
        kwargs['metavar'] = '[' + '|'.join(kwargs['choices']) + ']'

    AddOption('--' + name, **kwargs)

def get_option(name):
    return GetOption(name)

def has_option(name):
    optval = GetOption(name)
    # Options with nargs=0 are true when their value is the empty tuple. Otherwise,
    # if the value is falsish (empty string, None, etc.), coerce to False.
    return True if optval == () else bool(optval)

def use_system_version_of_library(name):
    return has_option('use-system-all') or has_option('use-system-' + name)

# Returns true if we have been configured to use a system version of any C++ library. If you
# add a new C++ library dependency that may be shimmed out to the system, add it to the below
# list.
def using_system_version_of_cxx_libraries():
    cxx_library_names = ["tcmalloc", "boost"]
    return True in [use_system_version_of_library(x) for x in cxx_library_names]

def make_variant_dir_generator():
    memoized_variant_dir = [False]
    def generate_variant_dir(target, source, env, for_signature):
        if not memoized_variant_dir[0]:
            memoized_variant_dir[0] = env.subst('$BUILD_ROOT/$VARIANT_DIR')
        return memoized_variant_dir[0]
    return generate_variant_dir

# Options TODOs:
#
# - We should either alphabetize the entire list of options, or split them into logical groups
#   with clear boundaries, and then alphabetize the groups. There is no way in SCons though to
#   inform it of options groups.
#
# - Many of these options are currently only either present or absent. This is not good for
#   scripting the build invocation because it means you need to interpolate in the presence of
#   the whole option. It is better to make all options take an optional on/off or true/false
#   using the nargs='const' mechanism.
#

add_option('prefix',
    default='$BUILD_ROOT/install',
    help='installation prefix',
)

add_option('nostrip',
    help='do not strip installed binaries',
    nargs=0,
)

add_option('build-dir',
    default='#build',
    help='build output directory',
)

add_option('release',
    help='release build',
    nargs=0,
)

add_option('lto',
    help='enable link time optimizations (experimental, except with MSVC)',
    nargs=0,
)

add_option('dynamic-windows',
    help='dynamically link on Windows',
    nargs=0,
)

add_option('endian',
    choices=['big', 'little', 'auto'],
    default='auto',
    help='endianness of target platform',
    nargs=1,
    type='choice',
)

add_option('disable-minimum-compiler-version-enforcement',
    help='allow use of unsupported older compilers (NEVER for production builds)',
    nargs=0,
)

add_option('ssl',
    help='Enable SSL',
    nargs=0
)

add_option('mmapv1',
    choices=['auto', 'on', 'off'],
    default='auto',
    help='Enable MMapV1',
    nargs='?',
    type='choice',
)

add_option('wiredtiger',
    choices=['on', 'off'],
    const='on',
    default='on',
    help='Enable wiredtiger',
    nargs='?',
    type='choice',
)

js_engine_choices = ['mozjs', 'none']
add_option('js-engine',
    choices=js_engine_choices,
    default=js_engine_choices[0],
    help='JavaScript scripting engine implementation',
    type='choice',
)

add_option('server-js',
    choices=['on', 'off'],
    const='on',
    default='on',
    help='Build mongod without JavaScript support',
    type='choice',
)

add_option('libc++',
    help='use libc++ (experimental, requires clang)',
    nargs=0,
)

add_option('use-glibcxx-debug',
    help='Enable the glibc++ debug implementations of the C++ standard libary',
    nargs=0,
)

add_option('noshell',
    help="don't build shell",
    nargs=0,
)

add_option('safeshell',
    help="don't let shell scripts run programs (still, don't run untrusted scripts)",
    nargs=0,
)

add_option('dbg',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Enable runtime debugging checks',
    nargs='?',
    type='choice',
)

add_option('spider-monkey-dbg',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Enable SpiderMonkey debug mode',
    nargs='?',
    type='choice',
)

add_option('opt',
    choices=['on', 'off'],
    const='on',
    help='Enable compile-time optimization',
    nargs='?',
    type='choice',
)

add_option('sanitize',
    help='enable selected sanitizers',
    metavar='san1,san2,...sanN',
)

add_option('llvm-symbolizer',
    default='llvm-symbolizer',
    help='name of (or path to) the LLVM symbolizer',
)

add_option('durableDefaultOn',
    help='have durable default to on',
    nargs=0,
)

add_option('allocator',
    choices=["auto", "system", "tcmalloc"],
    default="auto",
    help='allocator to use (use "auto" for best choice for current platform)',
    type='choice',
)

add_option('gdbserver',
    help='build in gdb server support',
    nargs=0,
)

add_option('gcov',
    help='compile with flags for gcov',
    nargs=0,
)

add_option('smokedbprefix',
    help='prefix to dbpath et al. for smoke tests',
)

add_option('smokeauth',
    help='run smoke tests with --auth',
    nargs=0,
)

add_option('use-sasl-client',
    help='Support SASL authentication in the client library',
    nargs=0,
)

add_option('use-system-tcmalloc',
    help='use system version of tcmalloc library',
    nargs=0,
)

add_option('use-system-pcre',
    help='use system version of pcre library',
    nargs=0,
)

add_option('use-system-wiredtiger',
    help='use system version of wiredtiger library',
    nargs=0,
)

add_option('system-boost-lib-search-suffixes',
    help='Comma delimited sequence of boost library suffixes to search',
)

add_option('use-system-boost',
    help='use system version of boost libraries',
    nargs=0,
)

add_option('use-system-snappy',
    help='use system version of snappy library',
    nargs=0,
)

add_option('use-system-valgrind',
    help='use system version of valgrind library',
    nargs=0,
)

add_option('use-system-zlib',
    help='use system version of zlib library',
    nargs=0,
)

add_option('use-system-stemmer',
    help='use system version of stemmer',
    nargs=0)

add_option('use-system-yaml',
    help='use system version of yaml',
    nargs=0,
)

add_option('use-system-asio',
    help="use system version of ASIO",
    nargs=0,
)

add_option('use-system-icu',
    help="use system version of ICU",
    nargs=0,
)

add_option('use-system-intel_decimal128',
    help='use system version of intel decimal128',
    nargs=0,
)

add_option('use-system-all',
    help='use all system libraries',
    nargs=0,
)

add_option('use-new-tools',
    help='put new tools in the tarball',
    nargs=0,
)

add_option('use-cpu-profiler',
    help='Link against the google-perftools profiler library',
    nargs=0,
)

add_option('build-fast-and-loose',
    choices=['on', 'off'],
    const='on',
    default='on',
    help='looser dependency checking, ignored for --release builds',
    nargs='?',
    type='choice',
)

add_option('disable-warnings-as-errors',
    help="Don't add -Werror to compiler command line",
    nargs=0,
)

add_option('variables-help',
    help='Print the help text for SCons variables',
    nargs=0,
)

add_option('osx-version-min',
    help='minimum OS X version to support',
)

win_version_min_choices = {
    'vista'   : ('0600', '0000'),
    'win7'    : ('0601', '0000'),
    'ws08r2'  : ('0601', '0000'),
    'win8'    : ('0602', '0000'),
    'win81'   : ('0603', '0000'),
}

add_option('win-version-min',
    choices=win_version_min_choices.keys(),
    default=None,
    help='minimum Windows version to support',
    type='choice',
)

add_option('cache',
    help='Use an object cache rather than a per-build variant directory (experimental)',
    nargs=0,
)

add_option('cache-dir',
    default='$BUILD_ROOT/scons/cache',
    help='Specify the directory to use for caching objects if --cache is in use',
)

add_option("cxx-std",
    choices=["11", "14"],
    default="11",
    help="Select the C++ langauge standard to build with",
)

def find_mongo_custom_variables():
    files = []
    for path in sys.path:
        probe = os.path.join(path, 'mongo_custom_variables.py')
        if os.path.isfile(probe):
            files.append(probe)
    return files

add_option('variables-files',
    default=find_mongo_custom_variables(),
    help="Specify variables files to load",
)

link_model_choices = ['auto', 'object', 'static', 'dynamic', 'dynamic-strict']
add_option('link-model',
    choices=link_model_choices,
    default='object',
    help='Select the linking model for the project',
    type='choice'
)

variable_parse_mode_choices=['auto', 'posix', 'other']
add_option('variable-parse-mode',
    choices=variable_parse_mode_choices,
    default=variable_parse_mode_choices[0],
    help='Select which parsing mode is used to interpret command line variables',
    type='choice',
)

add_option('modules',
    help="Comma-separated list of modules to build. Empty means none. Default is all.",
)

add_option('runtime-hardening',
    choices=["on", "off"],
    default="on",
    help="Enable runtime hardening features (e.g. stack smash protection)",
    type='choice',
)

try:
    with open("version.json", "r") as version_fp:
        version_data = json.load(version_fp)

    if 'version' not in version_data:
        print "version.json does not contain a version string"
        Exit(1)
    if 'githash' not in version_data:
        version_data['githash'] = utils.getGitVersion()

except IOError as e:
    # If the file error wasn't because the file is missing, error out
    if e.errno != errno.ENOENT:
        print "Error opening version.json: {0}".format(e.strerror)
        Exit(1)

    version_data = {
        'version': utils.getGitDescribe()[1:],
        'githash': utils.getGitVersion(),
    }

except ValueError as e:
    print "Error decoding version.json: {0}".format(e)
    Exit(1)

# Setup the command-line variables
def variable_shlex_converter(val):
    # If the argument is something other than a string, propogate
    # it literally.
    if not isinstance(val, basestring):
        return val
    parse_mode = get_option('variable-parse-mode')
    if parse_mode == 'auto':
        parse_mode = 'other' if is_running_os('windows') else 'posix'
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
    if is_running_os('windows'):
        # we only support MS toolchain on windows
        return ['msvc', 'mslink', 'mslib']
    elif is_running_os('linux', 'solaris'):
        return ['gcc', 'g++', 'gnulink', 'ar']
    elif is_running_os('osx'):
        return ['gcc', 'g++', 'applelink', 'ar']
    else:
        return ["default"]

def variable_tools_converter(val):
    tool_list = shlex.split(val)
    return tool_list + [
        "distsrc",
        "gziptool",
        "jsheader",
        "mergelib",
        "mongo_integrationtest",
        "mongo_unittest",
        "textfile",
    ]

def variable_distsrc_converter(val):
    if not val.endswith("/"):
        return val + "/"
    return val

variables_files = variable_shlex_converter(get_option('variables-files'))
for file in variables_files:
    print "Using variable customization file %s" % file

env_vars = Variables(
    files=variables_files,
    args=ARGUMENTS
)

env_vars.Add('ABIDW',
    help="Configures the path to the 'abidw' (a libabigail) utility")

env_vars.Add('ARFLAGS',
    help='Sets flags for the archiver',
    converter=variable_shlex_converter)

env_vars.Add('CC',
    help='Select the C compiler to use')

env_vars.Add('CCFLAGS',
    help='Sets flags for the C and C++ compiler',
    converter=variable_shlex_converter)

env_vars.Add('CFLAGS',
    help='Sets flags for the C compiler',
    converter=variable_shlex_converter)

env_vars.Add('CPPDEFINES',
    help='Sets pre-processor definitions for C and C++',
    converter=variable_shlex_converter,
    default=[])

env_vars.Add('CPPPATH',
    help='Adds paths to the preprocessor search path',
    converter=variable_shlex_converter)

env_vars.Add('CXX',
    help='Select the C++ compiler to use')

env_vars.Add('CXXFLAGS',
    help='Sets flags for the C++ compiler',
    converter=variable_shlex_converter)

# Note: This probably is only really meaningful when configured via a variables file. It will
# also override whatever the SCons platform defaults would be.
env_vars.Add('ENV',
    help='Sets the environment for subprocesses')

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

# Note: This is only really meaningful when configured via a variables file. See the
# default_buildinfo_environment_data() function for examples of how to use this.
env_vars.Add('MONGO_BUILDINFO_ENVIRONMENT_DATA',
    help='Sets the info returned from the buildInfo command and --version command-line flag',
    default=default_buildinfo_environment_data())

env_vars.Add('MONGO_DIST_SRC_PREFIX',
    help='Sets the prefix for files in the source distribution archive',
    converter=variable_distsrc_converter,
    default="mongodb-src-r${MONGO_VERSION}")

env_vars.Add('MONGO_DISTARCH',
    help='Adds a string representing the target processor architecture to the dist archive',
    default='$TARGET_ARCH')

env_vars.Add('MONGO_DISTMOD',
    help='Adds a string that will be embedded in the dist archive naming',
    default='')

env_vars.Add('MONGO_DISTNAME',
    help='Sets the version string to be used in dist archive naming',
    default='$MONGO_VERSION')

env_vars.Add('MONGO_VERSION',
    help='Sets the version string for MongoDB',
    default=version_data['version'])

env_vars.Add('MONGO_GIT_HASH',
    help='Sets the githash to store in the MongoDB version information',
    default=version_data['githash'])

env_vars.Add('MSVC_USE_SCRIPT',
    help='Sets the script used to setup Visual Studio.')

env_vars.Add('MSVC_VERSION',
    help='Sets the version of Visual Studio to use (e.g.  12.0, 11.0, 10.0)')

env_vars.Add('OBJCOPY',
    help='Sets the path to objcopy',
    default=WhereIs('objcopy'))

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

env_vars.Add('TARGET_OS',
    help='Sets the target OS to build for',
    default=get_running_os_name())

env_vars.Add('TOOLS',
    help='Sets the list of SCons tools to add to the environment',
    converter=variable_tools_converter,
    default=decide_platform_tools())

env_vars.Add('VARIANT_DIR',
    help='Sets the name (or generator function) for the variant directory',
    default=default_variant_dir_generator,
)

env_vars.Add('VERBOSE',
    help='Control build verbosity (auto, on/off true/false 1/0)',
    default='auto',
)

# -- Validate user provided options --

# A dummy environment that should *only* have the variables we have set. In practice it has
# some other things because SCons isn't quite perfect about keeping variable initialization
# scoped to Tools, but it should be good enough to do validation on any Variable values that
# came from the command line or from loaded files.
variables_only_env = Environment(
    # Disable platform specific variable injection
    platform=(lambda x: ()),
    # But do *not* load any tools, since those might actually set variables. Note that this
    # causes the value of our TOOLS variable to have no effect.
    tools=[],
    # Use the Variables specified above.
    variables=env_vars,
)

# don't run configure if user calls --help
if GetOption('help'):
    try:
        Help('\nThe following variables may also be set like scons VARIABLE=value\n', append=True)
        Help(env_vars.GenerateHelpText(variables_only_env), append=True)
    except TypeError:
        # The append=true kwarg is only supported in scons>=2.4. Without it, calls to Help() clobber
        # the automatically generated options help, which we don't want. Users on older scons
        # versions will need to use --variables-help to learn about which variables we support.
        pass

    Return()

if ('CC' in variables_only_env) != ('CXX' in variables_only_env):
    print('Cannot customize C compiler without customizing C++ compiler, and vice versa')
    Exit(1)

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

boostLibs = [ "thread" , "filesystem" , "program_options", "system", "regex", "chrono" ]

onlyServer = len( COMMAND_LINE_TARGETS ) == 0 or ( len( COMMAND_LINE_TARGETS ) == 1 and str( COMMAND_LINE_TARGETS[0] ) in [ "mongod" , "mongos" , "test" ] )

releaseBuild = has_option("release")

dbg_opt_mapping = {
    # --dbg, --opt   :   dbg    opt
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

usemozjs = (jsEngine.startswith('mozjs'))

if not serverJs and not usemozjs:
    print("Warning: --server-js=off is not needed with --js-engine=none")

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
               BUILD_DIR=make_variant_dir_generator(),
               DIST_ARCHIVE_SUFFIX='.tgz',
               DIST_BINARIES=[],
               MODULE_BANNERS=[],
               ARCHIVE_ADDITION_DIR_MAP={},
               ARCHIVE_ADDITIONS=[],
               PYTHON=utils.find_python(),
               SERVER_ARCHIVE='${SERVER_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
               UNITTEST_ALIAS='unittests',
               # TODO: Move unittests.txt to $BUILD_DIR, but that requires
               # changes to MCI.
               UNITTEST_LIST='$BUILD_ROOT/unittests.txt',
               INTEGRATION_TEST_ALIAS='integration_tests',
               INTEGRATION_TEST_LIST='$BUILD_ROOT/integration_tests.txt',
               CONFIGUREDIR=sconsDataDir.Dir('sconf_temp'),
               CONFIGURELOG=sconsDataDir.File('config.log'),
               INSTALL_DIR=installDir,
               CONFIG_HEADER_DEFINES={},
               LIBDEPS_TAG_EXPANSIONS=[],
               )

env = Environment(variables=env_vars, **envDict)
del envDict

env.AddMethod(env_os_is_wrapper, 'TargetOSIs')
env.AddMethod(env_get_os_name_wrapper, 'GetTargetOSName')

def fatal_error(env, msg, *args):
    print msg.format(*args)
    Exit(1)

def conf_error(env, msg, *args):
    print msg.format(*args)
    print "See {0} for details".format(env['CONFIGURELOG'].abspath)

    Exit(1)

env.AddMethod(fatal_error, 'FatalError')
env.AddMethod(conf_error, 'ConfError')

# Normalize the VERBOSE Option, and make its value available as a
# function.
if env['VERBOSE'] == "auto":
    env['VERBOSE'] = not sys.stdout.isatty()
elif env['VERBOSE'] in ('1', "ON", "on", "True", "true", True):
    env['VERBOSE'] = True
elif env['VERBOSE'] in ('0', "OFF", "off", "False", "false", False):
    env['VERBOSE'] = False
else:
    env.FatalError("Invalid value {0} for VERBOSE Variable", env['VERBOSE'])
env.AddMethod(lambda env: env['VERBOSE'], 'Verbose')

if has_option('variables-help'):
    print env_vars.GenerateHelpText(env)
    Exit(0)

unknown_vars = env_vars.UnknownVariables()
if unknown_vars:
    env.FatalError("Unknown variables specified: {0}", ", ".join(unknown_vars.keys()))

def set_config_header_define(env, varname, varval = 1):
    env['CONFIG_HEADER_DEFINES'][varname] = varval
env.AddMethod(set_config_header_define, 'SetConfigHeaderDefine')

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

endian = get_option( "endian" )

if endian == "auto":
    endian = sys.byteorder

if endian == "little":
    env.SetConfigHeaderDefine("MONGO_CONFIG_BYTE_ORDER", "1234")
elif endian == "big":
    env.SetConfigHeaderDefine("MONGO_CONFIG_BYTE_ORDER", "4321")

# These preprocessor macros came from
# http://nadeausoftware.com/articles/2012/02/c_c_tip_how_detect_processor_type_using_compiler_predefined_macros
#
# NOTE: Remember to add a trailing comma to form any required one
# element tuples, or your configure checks will fail in strange ways.
processor_macros = {
    'arm'     : { 'endian': 'little', 'defines': ('__arm__',) },
    'aarch64' : { 'endian': 'little', 'defines': ('__arm64__', '__aarch64__')},
    'i386'    : { 'endian': 'little', 'defines': ('__i386', '_M_IX86')},
    'ppc64le' : { 'endian': 'little', 'defines': ('__powerpc64__',)},
    's390x'   : { 'endian': 'big',    'defines': ('__s390x__',)},
    'sparc'   : { 'endian': 'big',    'defines': ('__sparc',)},
    'x86_64'  : { 'endian': 'little', 'defines': ('__x86_64', '_M_AMD64')},
}

def CheckForProcessor(context, which_arch):
    def run_compile_check(arch):
        full_macros = " || ".join([ "defined(%s)" % (v) for v in processor_macros[arch]['defines']])

        if not endian == processor_macros[arch]['endian']:
            return False

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

# Taken from http://nadeausoftware.com/articles/2012/01/c_c_tip_how_use_compiler_predefined_macros_detect_operating_system
os_macros = {
    "windows": "_WIN32",
    "solaris": "__sun",
    "freebsd": "__FreeBSD__",
    "openbsd": "__OpenBSD__",
    "osx": "__APPLE__",
    "linux": "__linux__",
}

def CheckForOS(context, which_os):
    test_body = """
    #if defined({0})
    /* detected {1} */
    #else
    #error
    #endif
    """.format(os_macros[which_os], which_os)
    context.Message('Checking if target OS {0} is supported by the toolchain '.format(which_os))
    ret = context.TryCompile(textwrap.dedent(test_body), ".c")
    context.Result(ret)
    return ret

def CheckForCXXLink(context):
    test_body = """
    #include <iostream>
    #include <cstdlib>

    int main() {
        std::cout << "Hello, World" << std::endl;
        return EXIT_SUCCESS;
    }
    """
    context.Message('Checking that the C++ compiler can link a C++ program... ')
    ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
    context.Result(ret)
    return ret

detectConf = Configure(detectEnv, help=False, custom_tests = {
    'CheckForToolchain' : CheckForToolchain,
    'CheckForProcessor': CheckForProcessor,
    'CheckForOS': CheckForOS,
    'CheckForCXXLink': CheckForCXXLink,
})

if not detectConf.CheckCC():
    env.ConfError(
        "C compiler {0} doesn't work",
        detectEnv['CC'])

if not detectConf.CheckCXX():
    env.ConfError(
        "C++ compiler {0} doesn't work",
        detectEnv['CXX'])

if not detectConf.CheckForCXXLink():
    env.ConfError(
        "C++ compiler {0} can't link C++ programs",
        detectEnv['CXX'])

toolchain_search_sequence = [ "GCC", "clang" ]
if is_running_os('windows'):
    toolchain_search_sequence = [ 'MSVC', 'clang', 'GCC' ]
for candidate_toolchain in toolchain_search_sequence:
    if detectConf.CheckForToolchain(candidate_toolchain, "C++", "CXX", ".cpp"):
        detected_toolchain = candidate_toolchain
        break

if not detected_toolchain:
    env.ConfError("Couldn't identify the C++ compiler")

if not detectConf.CheckForToolchain(detected_toolchain, "C", "CC", ".c"):
    env.ConfError("C compiler does not match identified C++ compiler")

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
        env.ConfError("Could not detect processor specified in TARGET_ARCH variable")
else:
    detected_processor = detectConf.CheckForProcessor(None)
    if not detected_processor:
        env.ConfError("Failed to detect a supported target architecture")
    env['TARGET_ARCH'] = detected_processor

if env['TARGET_OS'] not in os_macros:
    print "No special config for [{0}] which probably means it won't work".format(env['TARGET_OS'])
elif not detectConf.CheckForOS(env['TARGET_OS']):
    env.ConfError("TARGET_OS ({0}) is not supported by compiler", env['TARGET_OS'])

detectConf.Finish()

env['CC_VERSION'] = get_toolchain_ver(env, 'CC')
env['CXX_VERSION'] = get_toolchain_ver(env, 'CXX')

if not env['HOST_ARCH']:
    env['HOST_ARCH'] = env['TARGET_ARCH']

# In some places we have POSIX vs Windows cpp files, and so there's an additional
# env variable to interpolate their names in child sconscripts

env['TARGET_OS_FAMILY'] = 'posix' if env.TargetOSIs('posix') else env.GetTargetOSName()

# Currently we only use tcmalloc on windows and linux x86_64. Other
# linux targets (power, s390x, arm) do not currently support tcmalloc.
#
# Normalize the allocator option and store it in the Environment. It
# would be nicer to use SetOption here, but you can't reset user
# options for some strange reason in SCons. Instead, we store this
# option as a new variable in the environment.
if get_option('allocator') == "auto":
    if env.TargetOSIs('windows') or \
       env.TargetOSIs('linux'):
        env['MONGO_ALLOCATOR'] = "tcmalloc"
    else:
        env['MONGO_ALLOCATOR'] = "system"
else:
    env['MONGO_ALLOCATOR'] = get_option('allocator')

if has_option("cache"):
    if has_option("release"):
        env.FatalError(
            "Using the experimental --cache option is not permitted for --release builds")
    if has_option("gcov"):
        env.FatalError("Mixing --cache and --gcov doesn't work correctly yet. See SERVER-11084")
    env.CacheDir(str(env.Dir(cacheDir)))

# Normalize the link model. If it is auto, then a release build uses 'object' mode. Otherwise
# we automatically select the 'static' model on non-windows platforms, or 'object' if on
# Windows. If the user specified, honor the request, unless it conflicts with the requirement
# that release builds use the 'object' mode, in which case, error out.
#
# We require the use of the 'object' mode for release builds because it is the only linking
# model that works across all of our platforms. We would like to ensure that all of our
# released artifacts are built with the same known-good-everywhere model.
link_model = get_option('link-model')

if link_model == "auto":
    link_model = "object" if (env.TargetOSIs('windows') or has_option("release")) else "static"
elif has_option("release") and link_model != "object":
    env.FatalError("The link model for release builds is required to be 'object'")

# The only link model currently supported on Windows is 'object', since there is no equivalent
# to --whole-archive.
if env.TargetOSIs('windows') and link_model != 'object':
    env.FatalError("Windows builds must use the 'object' link model");

# The 'object' mode for libdeps is enabled by setting _LIBDEPS to $_LIBDEPS_OBJS. The other two
# modes operate in library mode, enabled by setting _LIBDEPS to $_LIBDEPS_LIBS.
env['_LIBDEPS'] = '$_LIBDEPS_OBJS' if link_model == "object" else '$_LIBDEPS_LIBS'

env['BUILDERS']['ProgramObject'] = env['BUILDERS']['StaticObject']
env['BUILDERS']['LibraryObject'] = env['BUILDERS']['StaticObject']

if link_model.startswith("dynamic"):

    # Add in the abi linking tool if the user requested and it is
    # supported on this platform.
    if env.get('ABIDW'):
        abilink = Tool('abilink')
        if abilink.exists(env):
            abilink(env)

    # Redirect the 'Library' target, which we always use instead of 'StaticLibrary' for things
    # that can be built in either mode, to point to SharedLibrary.
    env['BUILDERS']['Library'] = env['BUILDERS']['SharedLibrary']
    env['BUILDERS']['LibraryObject'] = env['BUILDERS']['SharedObject']

    # TODO: Ideally, the conditions below should be based on a
    # detection of what linker we are using, not the local OS, but I
    # doubt very much that we will see the mach-o linker on anything
    # other than Darwin, or a BFD/sun-esque linker elsewhere.

    # On Darwin, we need to tell the linker that undefined symbols are
    # resolved via dynamic lookup; otherwise we get build failures. On
    # other unixes, we need to suppress as-needed behavior so that
    # initializers are ensured present, even if there is no visible
    # edge to the library in the symbol graph.
    #
    # NOTE: The darwin linker flag is only needed because the library
    # graph is not a DAG. Once the graph is a DAG, we will require all
    # edges to be expressed, and we should drop the flag. When that
    # happens, we should also add -z,defs flag on ELF platforms to
    # ensure that missing symbols due to unnamed dependency edges
    # result in link errors.
    #
    # NOTE: The 'incomplete' tag can be applied to a library to
    # indicate that it does not (or cannot) completely express all of
    # its required link dependencies. This can occur for three
    # reasons:
    #
    # - No unique provider for the symbol: Some symbols do not have a
    #   unique dependency that provides a definition, in which case it
    #   is impossible for the library to express a dependency edge to
    #   resolve the symbol
    #
    # - The library is part of a cycle: If library A depends on B,
    #   which depends on C, which depends on A, then it is impossible
    #   to express all three edges in SCons, since otherwise there is
    #   no way to sequence building the libraries. The cyclic
    #   libraries actually work at runtime, because some parent object
    #   links all of them.
    #
    # - The symbol is provided by an executable into which the library
    #   will be linked. The mongo::inShutdown symbol is a good
    #   example.
    #
    # All of these are defects in the linking model. In an effort to
    # eliminate these issues, we have begun tagging those libraries
    # that are affected, and requiring that all non-tagged libraries
    # correctly express all dependencies. As we repair each defective
    # library, we can remove the tag. When all the tags are removed
    # the graph will be acyclic.

    if env.TargetOSIs('osx'):
        if link_model == "dynamic-strict":
            # Darwin is strict by default
            pass
        else:
            def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                # On darwin, since it is strict by default, we need to add a flag
                # when libraries are tagged incomplete.
                if 'incomplete' in target[0].get_env().get("LIBDEPS_TAGS", []):
                    return ["-Wl,-undefined,dynamic_lookup"]
                return []
            env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)
    else:
        env.AppendUnique(SHLINKFLAGS=["-Wl,--no-as-needed"])

        # Using zdefs doesn't work at all with the sanitizers
        if not has_option('sanitize'):

            if link_model == "dynamic-strict":
                env.AppendUnique(SHLINKFLAGS=["-Wl,-z,defs"])
            else:
                # On BFD/gold linker environments, which are not strict by
                # default, we need to add a flag when libraries are not
                # tagged incomplete.
                def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                    if 'incomplete' not in target[0].get_env().get("LIBDEPS_TAGS", []):
                        return ["-Wl,-z,defs"]
                    return []
                env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)

if optBuild:
    env.SetConfigHeaderDefine("MONGO_CONFIG_OPTIMIZED_BUILD")

# Ignore requests to build fast and loose for release builds.
# Also ignore fast-and-loose option if the scons cache is enabled (see SERVER-19088)
if get_option('build-fast-and-loose') == "on" and \
    not has_option('release') and not has_option('cache'):
    # See http://www.scons.org/wiki/GoFastButton for details
    env.Decider('MD5-timestamp')
    env.SetOption('max_drift', 1)

# On non-windows platforms, we may need to differentiate between flags being used to target an
# executable (like -fPIE), vs those being used to target a (shared) library (like -fPIC). To do so,
# we inject a new family of SCons variables PROG*FLAGS, by reaching into the various COMs.
if not env.TargetOSIs('windows'):
    env["CCCOM"] = env["CCCOM"].replace("$CFLAGS", "$CFLAGS $PROGCFLAGS")
    env["CXXCOM"] = env["CXXCOM"].replace("$CXXFLAGS", "$CXXFLAGS $PROGCXXFLAGS")
    env["CCCOM"] = env["CCCOM"].replace("$CCFLAGS", "$CCFLAGS $PROGCCFLAGS")
    env["CXXCOM"] = env["CXXCOM"].replace("$CCFLAGS", "$CCFLAGS $PROGCCFLAGS")
    env["LINKCOM"] = env["LINKCOM"].replace("$LINKFLAGS", "$LINKFLAGS $PROGLINKFLAGS")

if not env.Verbose():
    env.Append( CCCOMSTR = "Compiling $TARGET" )
    env.Append( CXXCOMSTR = env["CCCOMSTR"] )
    env.Append( SHCCCOMSTR = "Compiling $TARGET" )
    env.Append( SHCXXCOMSTR = env["SHCCCOMSTR"] )
    env.Append( LINKCOMSTR = "Linking $TARGET" )
    env.Append( SHLINKCOMSTR = env["LINKCOMSTR"] )
    env.Append( ARCOMSTR = "Generating library $TARGET" )

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

libdeps.setup_environment(env, emitting_shared=(link_model.startswith("dynamic")))

if env.TargetOSIs('linux', 'freebsd', 'openbsd'):
    env['LINK_LIBGROUP_START'] = '-Wl,--start-group'
    env['LINK_LIBGROUP_END'] = '-Wl,--end-group'
    env['LINK_WHOLE_ARCHIVE_START'] = '-Wl,--whole-archive'
    env['LINK_WHOLE_ARCHIVE_END'] = '-Wl,--no-whole-archive'
elif env.TargetOSIs('osx'):
    env['LINK_LIBGROUP_START'] = ''
    env['LINK_LIBGROUP_END'] = ''
    env['LINK_WHOLE_ARCHIVE_START'] = '-Wl,-all_load'
    env['LINK_WHOLE_ARCHIVE_END'] = '-Wl,-noall_load'
elif env.TargetOSIs('solaris'):
    env['LINK_LIBGROUP_START'] = '-z rescan-start'
    env['LINK_LIBGROUP_END'] = '-z rescan-end'
    env['LINK_WHOLE_ARCHIVE_START'] = '-z allextract'
    env['LINK_WHOLE_ARCHIVE_END'] = '-z defaultextract'

# ---- other build setup -----
if debugBuild:
    env.SetConfigHeaderDefine("MONGO_CONFIG_DEBUG_BUILD")
else:
    env.AppendUnique( CPPDEFINES=[ 'NDEBUG' ] )

if env.TargetOSIs('linux'):
    env.Append( LIBS=['m'] )

elif env.TargetOSIs('solaris'):
     env.Append( LIBS=["socket","resolv","lgrp"] )

elif env.TargetOSIs('freebsd'):
    env.Append( LIBS=[ "kvm" ] )
    env.Append( CCFLAGS=[ "-fno-omit-frame-pointer" ] )

elif env.TargetOSIs('openbsd'):
    env.Append( LIBS=[ "kvm" ] )

elif env.TargetOSIs('windows'):
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

    env.Append(CPPDEFINES=[
    # This tells the Windows compiler not to link against the .lib files
    # and to use boost as a bunch of header-only libraries
        "BOOST_ALL_NO_LIB",
    ])

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
    # c4013
    #  'function' undefined; assuming extern returning int
    #    This warning occurs when files compiled for the C language use functions not defined
    #    in a header file.
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
    env.Append( CCFLAGS=["/we4013", "/we4099", "/we4930"] )

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

    # Support large object files since some unit-test sources contain a lot of code
    env.Append( CCFLAGS=["/bigobj"] )

    # This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds
    env.Append( LINKFLAGS=["/LARGEADDRESSAWARE"] )

    env.Append(
        LIBS=[
            'DbgHelp.lib',
            'Iphlpapi.lib',
            'Psapi.lib',
            'advapi32.lib',
            'bcrypt.lib',
            'crypt32.lib',
            'kernel32.lib',
            'shell32.lib',
            'version.lib',
            'winmm.lib',
            'ws2_32.lib',
        ],
    )

# When building on visual studio, this sets the name of the debug symbols file
if env.ToolchainIs('msvc'):
    env['PDB'] = '${TARGET.base}.pdb'

if env.TargetOSIs('posix'):

    # Everything on OS X is position independent by default. Solaris doesn't support PIE.
    if not env.TargetOSIs('osx', 'solaris'):
        if get_option('runtime-hardening') == "on":
            # If runtime hardening is requested, then build anything
            # destined for an executable with the necessary flags for PIE.
            env.AppendUnique(
                PROGCCFLAGS=['-fPIE'],
                PROGLINKFLAGS=['-pie'],
            )

    # -Winvalid-pch Warn if a precompiled header (see Precompiled Headers) is found in the search path but can't be used.
    env.Append( CCFLAGS=["-fno-omit-frame-pointer",
                         "-fno-strict-aliasing",
                         "-ggdb",
                         "-pthread",
                         "-Wall",
                         "-Wsign-compare",
                         "-Wno-unknown-pragmas",
                         "-Winvalid-pch"] )
    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    if env.TargetOSIs('linux', 'osx', 'solaris'):
        if not has_option("disable-warnings-as-errors"):
            env.Append( CCFLAGS=["-Werror"] )

    env.Append( CXXFLAGS=["-Woverloaded-virtual"] )
    env.Append( LINKFLAGS=["-pthread"] )

    # SERVER-9761: Ensure early detection of missing symbols in dependent libraries at program
    # startup.
    if env.TargetOSIs('osx'):
        env.Append( LINKFLAGS=["-Wl,-bind_at_load"] )
    else:
        env.Append( LINKFLAGS=["-Wl,-z,now"] )
        env.Append( LINKFLAGS=["-rdynamic"] )

    env.Append( LIBS=[] )

    #make scons colorgcc friendly
    for key in ('HOME', 'TERM'):
        try:
            env['ENV'][key] = os.environ[key]
        except KeyError:
            pass

    if env.TargetOSIs('linux') and has_option( "gcov" ):
        env.Append( CCFLAGS=["-fprofile-arcs", "-ftest-coverage"] )
        env.Append( LINKFLAGS=["-fprofile-arcs", "-ftest-coverage"] )

    if optBuild:
        env.Append( CCFLAGS=["-O2"] )
    else:
        env.Append( CCFLAGS=["-O0"] )

    # Promote linker warnings into errors. We can't yet do this on OS X because its linker considers
    # noall_load obsolete and warns about it.
    if not env.TargetOSIs('osx'):
        env.Append(
            LINKFLAGS=[
                "-Wl,--fatal-warnings",
            ],
        )

mmapv1 = False
if get_option('mmapv1') == 'auto':
    # The mmapv1 storage engine is only supported on x86
    # targets. Unless explicitly requested, disable it on all other
    # platforms.
    mmapv1 = (env['TARGET_ARCH'] in ['i386', 'x86_64'])
elif get_option('mmapv1') == 'on':
    mmapv1 = True

wiredtiger = False
if get_option('wiredtiger') == 'on':
    # Wiredtiger only supports 64-bit architecture, and will fail to compile on 32-bit
    # so disable WiredTiger automatically on 32-bit since wiredtiger is on by default
    if env['TARGET_ARCH'] == 'i386':
        env.FatalError("WiredTiger is not supported on 32-bit platforms\n"
            "Re-run scons with --wiredtiger=off to build on 32-bit platforms")
    else:
        wiredtiger = True
        env.SetConfigHeaderDefine("MONGO_CONFIG_WIREDTIGER_ENABLED")

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

# Needed for auth tests since key files are stored in git with mode 644.
if not env.TargetOSIs('windows'):
    for keysuffix in [ "1" , "2" ]:
        keyfile = "jstests/libs/key%s" % keysuffix
        os.chmod( keyfile , stat.S_IWUSR|stat.S_IRUSR )

# boostSuffixList is used when using system boost to select a search sequence
# for boost libraries.
boostSuffixList = ["-mt", ""]
if get_option("system-boost-lib-search-suffixes") is not None:
    if not use_system_version_of_library("boost"):
        env.FatalError("The --system-boost-lib-search-suffixes option is only valid "
            "with --use-system-boost")
    boostSuffixList = get_option("system-boost-lib-search-suffixes")
    if boostSuffixList == "":
        boostSuffixList = []
    else:
        boostSuffixList = boostSuffixList.split(',')

# discover modules, and load the (python) module for each module's build.py
mongo_modules = moduleconfig.discover_modules('src/mongo/db/modules', get_option('modules'))
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
        compiler_minimum_string = "Microsoft Visual Studio 2015 Update 2"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(_MSC_VER)
        #error
        #endif

        #if _MSC_VER < 1900 || (_MSC_VER == 1900 && _MSC_FULL_VER < 190023918)
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('gcc'):
        compiler_minimum_string = "GCC 5.3.0"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(__GNUC__) || defined(__clang__)
        #error
        #endif

        #if (__GNUC__ < 5) || (__GNUC__ == 5 && __GNUC_MINOR__ < 3) || (__GNUC__ == 5 && __GNUC_MINOR__ == 3 && __GNUC_PATCHLEVEL__ < 0)
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
        myenv.ConfError("Error: can't check compiler minimum; don't know this compiler...")

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
        env.FatalError("--disable-minimum-compiler-version-enforcement is forbidden with --release")

    if not (c_compiler_validated and cxx_compiler_validated):
        if not suppress_invalid:
            env.ConfError("ERROR: Refusing to build with compiler that does not meet requirements")
        print("WARNING: Ignoring failed compiler version check per explicit user request.")
        print("WARNING: The build may fail, binaries may crash, or may run but corrupt data...")

    # Figure out what our minimum windows version is. If the user has specified, then use
    # that.
    if env.TargetOSIs('windows'):
        if has_option('win-version-min'):
            win_version_min = get_option('win-version-min')
        else:
            # If no minimum version has beeen specified, use our default
            win_version_min = 'vista'

        env['WIN_VERSION_MIN'] = win_version_min
        win_version_min = win_version_min_choices[win_version_min]
        env.Append( CPPDEFINES=[("_WIN32_WINNT", "0x" + win_version_min[0])] )
        env.Append( CPPDEFINES=[("NTDDI_VERSION", "0x" + win_version_min[0] + win_version_min[1])] )

    conf.Finish()

    def AddFlagIfSupported(env, tool, extension, flag, link, **mutation):
        def CheckFlagTest(context, tool, extension, flag):
            if link:
                if tool == 'C':
                    test_body = """
                    #include <stdlib.h>
                    #include <stdio.h>
                    int main() {
                        printf("Hello, World!");
                        return EXIT_SUCCESS;
                    }"""
                elif tool == 'C++':
                    test_body = """
                    #include <iostream>
                    #include <cstdlib>
                    int main() {
                        std::cout << "Hello, World!" << std::endl;
                        return EXIT_SUCCESS;
                    }"""
                context.Message('Checking if linker supports %s... ' % (flag))
                ret = context.TryLink(textwrap.dedent(test_body), extension)
            else:
                test_body = ""
                context.Message('Checking if %s compiler supports %s... ' % (tool, flag))
                ret = context.TryCompile(textwrap.dedent(test_body), extension)
            context.Result(ret)
            return ret

        if env.ToolchainIs('msvc'):
            env.FatalError("AddFlagIfSupported is not currently supported with MSVC")

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
        return AddFlagIfSupported(env, 'C', '.c', flag, False, CFLAGS=[flag])

    def AddToCCFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, False, CCFLAGS=[flag])

    def AddToCXXFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C++', '.cpp', flag, False, CXXFLAGS=[flag])

    def AddToLINKFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, True, LINKFLAGS=[flag])

    def AddToSHLINKFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, True, SHLINKFLAGS=[flag])


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

        # Suppress warnings about not consistently using override everywhere in a class. It seems
        # very pedantic, and we have a fair number of instances.
        AddToCCFLAGSIfSupported(myenv, "-Wno-inconsistent-missing-override")

        # Don't issue warnings about potentially evaluated expressions
        AddToCCFLAGSIfSupported(myenv, "-Wno-potentially-evaluated-expression")

        # Warn about moves of prvalues, which can inhibit copy elision.
        AddToCXXFLAGSIfSupported(myenv, "-Wpessimizing-move")

        # Warn about redundant moves, such as moving a local variable in a return that is different
        # than the return type.
        AddToCXXFLAGSIfSupported(myenv, "-Wredundant-move")

        # Disable warning about variables that may not be initialized
        # Failures are triggered in the case of boost::optional in GCC 4.8.x
        # TODO: re-evaluate when we move to GCC 5.3
        # see: http://stackoverflow.com/questions/21755206/how-to-get-around-gcc-void-b-4-may-be-used-uninitialized-in-this-funct
        AddToCXXFLAGSIfSupported(myenv, "-Wno-maybe-uninitialized")

        # Check if we can set "-Wnon-virtual-dtor" when "-Werror" is set. The only time we can't set it is on
        # clang 3.4, where a class with virtual function(s) and a non-virtual destructor throws a warning when
        # it shouldn't.
        def CheckNonVirtualDtor(context):

            test_body = """
            class Base {
            public:
                virtual void foo() const = 0;
            protected:
                ~Base() {};
            };

            class Derived : public Base {
            public:
                virtual void foo() const {}
            };
            """

            context.Message('Checking -Wnon-virtual-dtor for false positives... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        myenvClone = myenv.Clone()
        myenvClone.Append( CCFLAGS=['-Werror'] )
        myenvClone.Append( CXXFLAGS=["-Wnon-virtual-dtor"] )
        conf = Configure(myenvClone, help=False, custom_tests = {
            'CheckNonVirtualDtor' : CheckNonVirtualDtor,
        })
        if conf.CheckNonVirtualDtor():
            myenv.Append( CXXFLAGS=["-Wnon-virtual-dtor"] )
        conf.Finish()

    if get_option('runtime-hardening') == "on":
        # Clang honors these flags, but doesn't actually do anything with them for compatibility, so we
        # need to only do this for GCC. On clang, we do things differently. Note that we need to add
        # these to the LINKFLAGS as well, since otherwise we might not link libssp when we need to (see
        # SERVER-12456).
        if myenv.ToolchainIs('gcc'):
            if AddToCCFLAGSIfSupported(myenv, '-fstack-protector-strong'):
                myenv.Append(
                    LINKFLAGS=[
                        '-fstack-protector-strong',
                    ]
                )
            elif AddToCCFLAGSIfSupported(myenv, '-fstack-protector-all'):
                myenv.Append(
                    LINKFLAGS=[
                        '-fstack-protector-all',
                    ]
                )
        elif myenv.ToolchainIs('clang'):
            # TODO: Clang stack hardening. There are several interesting
            # things to try here, but they each have consequences we need
            # to investigate.
            #
            # - fsanitize=bounds: This does static bounds checking. We can
            #   probably turn this on along with fsanitize-trap so that we
            #   don't depend on the ASAN runtime.
            #
            # - fsanitize=safestack: This looks very interesting, and is
            #   probably what we want. However there are a few problems:
            #
            #   - It relies on having the RT library available, and it is
            #     unclear whether we can ship binaries that depend on
            #     that.
            #
            #   - It is incompatible with a shared object build.
            #
            #   - It may not work with SpiderMonkey due to needing to
            #     inform the GC about the stacks so that mark-sweep
            #
            # - fsanitize=cfi: Again, very interesting, however it
            #   requires LTO builds.
            pass

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
    if env.TargetOSIs('osx') and has_option('osx-version-min'):
        min_version = get_option('osx-version-min')
        min_version_flag = '-mmacosx-version-min=%s' % (min_version)
        if not AddToCCFLAGSIfSupported(myenv, min_version_flag):
            myenv.ConfError("Can't set minimum OS X version with this compiler")
        myenv.AppendUnique(LINKFLAGS=[min_version_flag])

    usingLibStdCxx = False
    if has_option('libc++'):
        if not myenv.ToolchainIs('clang'):
            myenv.FatalError('libc++ is currently only supported for clang')
        if env.TargetOSIs('osx') and has_option('osx-version-min') and versiontuple(min_version) < versiontuple('10.7'):
            print("Warning: You passed option 'libc++'. You probably want to also pass 'osx-version-min=10.7' or higher for libc++ support.")
        if AddToCXXFLAGSIfSupported(myenv, '-stdlib=libc++'):
            myenv.Append(LINKFLAGS=['-stdlib=libc++'])
        else:
            myenv.ConfError('libc++ requested, but compiler does not support -stdlib=libc++' )
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
        if get_option('cxx-std') == "11":
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++11'):
                myenv.ConfError('Compiler does not honor -std=c++11')
        elif get_option('cxx-std') == "14":
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++14'):
                myenv.ConfError('Compiler does not honor -std=c++14')
        if not AddToCFLAGSIfSupported(myenv, '-std=c99'):
            myenv.ConfError("C++11 mode selected for C++ files, but can't enable C99 for C files")

    if using_system_version_of_cxx_libraries():
        print( 'WARNING: System versions of C++ libraries must be compiled with C++11/14 support' )

    # We appear to have C++11, or at least a flag to enable it. Check that the declared C++
    # language level is not less than C++11, and that we can at least compile an 'auto'
    # expression. We don't check the __cplusplus macro when using MSVC because as of our
    # current required MS compiler version (MSVS 2013 Update 4), they don't set it. If
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

    def CheckCxx14(context):
        test_body = """
        #ifndef _MSC_VER
        #if __cplusplus < 201402L
        #error
        #endif
        #endif
        auto DeducedReturnTypesAreACXX14Feature() {
            return 0;
        }
        """

        context.Message('Checking for C++14... ')
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCxx11' : CheckCxx11,
        'CheckCxx14' : CheckCxx14,
    })

    if not conf.CheckCxx11():
        myenv.ConfError('C++11 support is required to build MongoDB')

    if get_option('cxx-std') == "14":
        if not conf.CheckCxx14():
            myenv.ConfError('C++14 does not appear to work with the current toolchain')

    conf.Finish()

    def CheckMemset_s(context):
        test_body = """
        #define __STDC_WANT_LIB_EXT1__ 1
        #include <cstring>
        int main(int argc, char* argv[]) {
            void* data = nullptr;
            return memset_s(data, 0, 0, 0);
        }
        """

        context.Message('Checking for memset_s... ')
        ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(env, custom_tests = {
        'CheckMemset_s' : CheckMemset_s,
    })
    if conf.CheckMemset_s():
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_MEMSET_S")

    if conf.CheckFunc('strnlen'):
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_STRNLEN")

    conf.Finish()

    # If we are using libstdc++, check to see if we are using a
    # libstdc++ that is older than our GCC minimum of 5.3.0. This is
    # primarly to help people using clang on OS X but forgetting to
    # use --libc++ (or set the target OS X version high enough to get
    # it as the default). We would, ideally, check the __GLIBCXX__
    # version, but for various reasons this is not workable. Instead,
    # we switch on the fact that the <experimental/filesystem> header
    # wasn't introduced until libstdc++ 5.3.0. Yes, this is a terrible
    # hack.
    if usingLibStdCxx:
        def CheckModernLibStdCxx(context):
            test_body = """
            #if !__has_include(<experimental/filesystem>)
            #error "libstdc++ from GCC 5.3.0 or newer is required"
            #endif
            """

            context.Message('Checking for libstdc++ 5.3.0 or better... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckModernLibStdCxx' : CheckModernLibStdCxx,
        })

        suppress_invalid = has_option("disable-minimum-compiler-version-enforcement")
        if not conf.CheckModernLibStdCxx() and not suppress_invalid:
            myenv.ConfError("When using libstdc++, MongoDB requires libstdc++ from GCC 5.3.0 or newer")

        conf.Finish()

    if has_option("use-glibcxx-debug"):
        # If we are using a modern libstdc++ and this is a debug build and we control all C++
        # dependencies, then turn on the debugging features in libstdc++.
        # TODO: Need a new check here.
        if not debugBuild:
            myenv.FatalError("--use-glibcxx-debug requires --dbg=on")
        if not usingLibStdCxx:
            myenv.FatalError("--use-glibcxx-debug is only compatible with the GNU implementation "
                "of the C++ standard libary")
        if using_system_version_of_cxx_libraries():
            myenv.FatalError("--use-glibcxx-debug not compatible with system versions of "
                "C++ libraries.")
        myenv.Append(CPPDEFINES=["_GLIBCXX_DEBUG"]);

    # Check if we have a modern Windows SDK
    if env.TargetOSIs('windows'):
        def CheckWindowsSDKVersion(context):

            test_body = """
            #include <windows.h>
            #if !defined(NTDDI_WINBLUE)
            #error Need Windows SDK Version 8.1 or higher
            #endif
            """

            context.Message('Checking Windows SDK is 8.1 or newer... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckWindowsSDKVersion' : CheckWindowsSDKVersion,
        })

        if not conf.CheckWindowsSDKVersion():
            myenv.ConfError('Windows SDK Version 8.1 or higher is required to build MongoDB')

        conf.Finish()

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

        # On 32-bit systems, we need to define this in order to get access to
        # the 64-bit versions of fseek, etc.
        if not conf.CheckTypeSize('off_t', includes="#include <sys/types.h>", expect=8):
            myenv.Append(CPPDEFINES=["_FILE_OFFSET_BITS=64"])

        conf.Finish()

    if has_option('sanitize'):

        if not myenv.ToolchainIs('clang', 'gcc'):
            env.FatalError('sanitize is only supported with clang or gcc')

        if env['MONGO_ALLOCATOR'] == 'tcmalloc':
            # There are multiply defined symbols between the sanitizer and
            # our vendorized tcmalloc.
            env.FatalError("Cannot use --sanitize with tcmalloc")

        sanitizer_list = get_option('sanitize').split(',')

        using_lsan = 'leak' in sanitizer_list
        using_asan = 'address' in sanitizer_list or using_lsan
        using_tsan = 'thread' in sanitizer_list
        using_ubsan = 'undefined' in sanitizer_list

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
            myenv.ConfError('Failed to enable sanitizers with flag: {0}', sanitizer_option )

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

        tsan_options = ""
        if llvm_symbolizer:
            myenv['ENV']['ASAN_SYMBOLIZER_PATH'] = llvm_symbolizer
            myenv['ENV']['LSAN_SYMBOLIZER_PATH'] = llvm_symbolizer
            tsan_options = "external_symbolizer_path=\"%s\" " % llvm_symbolizer
        elif using_lsan:
            myenv.FatalError("Using the leak sanitizer requires a valid symbolizer")

        if using_tsan:
            tsan_options += "suppressions=\"%s\" " % myenv.File("#etc/tsan.suppressions").abspath
            myenv['ENV']['TSAN_OPTIONS'] = tsan_options

        if using_ubsan:
            # By default, undefined behavior sanitizer doesn't stop on
            # the first error. Make it so. Newer versions of clang
            # have renamed the flag.
            if not AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover"):
                AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover=undefined")

            # Ideally, we would apply this only in the WiredTiger
            # directory until WT-2631 is resolved, but we can't rely
            # on the flag being supported until clang-3.6, which isn't
            # our minimum, and we don't have access to
            # AddToCCFFLAGSIfSupported in the scope of the WT
            # Sconscript.
            #
            AddToCCFLAGSIfSupported(myenv, "-fno-sanitize=nonnull-attribute")

    if myenv.ToolchainIs('msvc') and optBuild:
        # http://blogs.msdn.com/b/vcblog/archive/2013/09/11/introducing-gw-compiler-switch.aspx
        #
        myenv.Append( CCFLAGS=["/Gw", "/Gy"] )
        myenv.Append( LINKFLAGS=["/OPT:REF"])

        # http://blogs.msdn.com/b/vcblog/archive/2014/03/25/linker-enhancements-in-visual-studio-2013-update-2-ctp2.aspx
        #
        myenv.Append( CCFLAGS=["/Zc:inline"])

    if myenv.ToolchainIs('gcc', 'clang'):
        # This tells clang/gcc to use the gold linker if it is available - we prefer the gold linker
        # because it is much faster.
        AddToLINKFLAGSIfSupported(myenv, '-fuse-ld=gold')

        # Explicitly enable GNU build id's if the linker supports it.
        AddToLINKFLAGSIfSupported(myenv, '-Wl,--build-id')

        # Disallow an executable stack. Also, issue a warning if any files are found that would
        # cause the stack to become executable if the noexecstack flag was not in play, so that we
        # can find them and fix them. We do this here after we check for ld.gold because the
        # --warn-execstack is currently only offered with gold.
        #
        # TODO: Add -Wl,--fatal-warnings once WT-2629 is fixed. We probably can do that
        # unconditionally above, and not need to do it as an AddToLINKFLAGSIfSupported step, since
        # both gold and binutils ld both support it.
        AddToLINKFLAGSIfSupported(myenv, "-Wl,-z,noexecstack")
        AddToLINKFLAGSIfSupported(myenv, "-Wl,--warn-execstack")

        # If possible with the current linker, mark relocations as read-only.
        AddToLINKFLAGSIfSupported(myenv, "-Wl,-z,relro")

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
            if not AddToCCFLAGSIfSupported(myenv, '-flto') or \
                    not AddToLINKFLAGSIfSupported(myenv, '-flto'):
                myenv.ConfError("Link time optimization requested, "
                    "but selected compiler does not honor -flto" )
        else:
            myenv.ConfError("Don't know how to enable --lto on current toolchain")

    if get_option('runtime-hardening') == "on":
        # Older glibc doesn't work well with _FORTIFY_SOURCE=2. Selecting 2.11 as the minimum was an
        # emperical decision, as that is the oldest non-broken glibc we seem to require. It is possible
        # that older glibc's work, but we aren't trying.
        #
        # https://gforge.inria.fr/tracker/?func=detail&group_id=131&atid=607&aid=14070
        # https://github.com/jedisct1/libsodium/issues/202
        def CheckForGlibcKnownToSupportFortify(context):
            test_body="""
            #include <features.h>
            #if !__GLIBC_PREREQ(2, 11)
            #error
            #endif
            """
            context.Message('Checking for glibc with non-broken _FORTIFY_SOURCE...')
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests = {
            'CheckForFortify': CheckForGlibcKnownToSupportFortify,
        })

        # Fortify only possibly makes sense on POSIX systems, and we know that clang is not a valid
        # combination:
        #
        # http://lists.llvm.org/pipermail/cfe-dev/2015-November/045852.html
        #
        if env.TargetOSIs('posix') and not env.ToolchainIs('clang') and conf.CheckForFortify():
            conf.env.Append(
                CPPDEFINES=[
                    ('_FORTIFY_SOURCE', 2),
                ],
            )

        myenv = conf.Finish()

    # We set this to work around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=43052
    if not myenv.ToolchainIs('msvc'):
        AddToCCFLAGSIfSupported(myenv, "-fno-builtin-memcmp")

    def CheckStorageClass(context, storage_class):
        test_body = """
        {0} int tsp_int = 1;
        int main(int argc, char** argv) {{
            return !(tsp_int == argc);
        }}
        """.format(storage_class)
        context.Message('Checking for storage class {0} '.format(storage_class))
        ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckStorageClass': CheckStorageClass
    })
    haveTriviallyConstructibleThreadLocals = False
    for storage_class, macro_name in [
            ('thread_local', 'MONGO_CONFIG_HAVE_THREAD_LOCAL'),
            ('__thread', 'MONGO_CONFIG_HAVE___THREAD'),
            ('__declspec(thread)', 'MONGO_CONFIG_HAVE___DECLSPEC_THREAD')]:
        if conf.CheckStorageClass(storage_class):
            haveTriviallyConstructibleThreadLocals = True
            myenv.SetConfigHeaderDefine(macro_name)
    conf.Finish()
    if not haveTriviallyConstructibleThreadLocals:
        env.ConfError("Compiler must support a thread local storage class for trivially constructible types")

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
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_STD_IS_TRIVIALLY_COPYABLE")

    myenv = conf.Finish()

    def CheckCXX14EnableIfT(context):
        test_body = """
        #include <cstdlib>
        #include <type_traits>

        template <typename = void>
        struct scons {
            bool hasSupport() { return false; }
        };

        template <>
        struct scons<typename std::enable_if_t<true>> {
            bool hasSupport() { return true; }
        };

        int main(int argc, char **argv) {
            scons<> SCons;
            return SCons.hasSupport() ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        """
        context.Message('Checking for C++14 std::enable_if_t support...')
        ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
        context.Result(ret)
        return ret

    # Check for std::enable_if_t support without using the __cplusplus macro
    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCXX14EnableIfT' : CheckCXX14EnableIfT,
    })

    if conf.CheckCXX14EnableIfT():
        conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_STD_ENABLE_IF_T')

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
        conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_STD_MAKE_UNIQUE')

    myenv = conf.Finish()

    def CheckCXX11Align(context):
        test_body = """
        #include <memory>
        int main(int argc, char **argv) {
            char buf[100];
            void* ptr = static_cast<void*>(buf);
            std::size_t size = sizeof(buf);
            auto foo = std::align(16, 16, ptr, size);
            return 0;
        }
        """
        context.Message('Checking for C++11 std::align support... ')
        ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
        context.Result(ret)
        return ret

    # Check for std::align support
    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCXX11Align': CheckCXX11Align,
    })

    if conf.CheckCXX11Align():
        conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_STD_ALIGN')

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

    if has_option( "ssl" ):
        sslLibName = "ssl"
        cryptoLibName = "crypto"
        if conf.env.TargetOSIs('windows'):
            sslLibName = "ssleay32"
            cryptoLibName = "libeay32"

        # Used to import system certificate keychains
        if conf.env.TargetOSIs('osx'):
            conf.env.AppendUnique(FRAMEWORKS=[
                'CoreFoundation',
                'Security',
            ])

        if not conf.CheckLibWithHeader(
                sslLibName,
                ["openssl/ssl.h"],
                "C",
                "SSL_version(NULL);",
                autoadd=True):
            conf.env.ConfError("Couldn't find OpenSSL ssl.h header and library")

        if not conf.CheckLibWithHeader(
                cryptoLibName,
                ["openssl/crypto.h"],
                "C",
                "SSLeay_version(0);",
                autoadd=True):
            conf.env.ConfError("Couldn't find OpenSSL crypto.h header and library")

        def CheckLinkSSL(context):
            test_body = """
            #include <openssl/err.h>
            #include <openssl/ssl.h>
            #include <stdlib.h>

            int main() {
                SSL_library_init();
                SSL_load_error_strings();
                ERR_load_crypto_strings();

                OpenSSL_add_all_algorithms();
                ERR_free_strings();

                return EXIT_SUCCESS;
            }
            """
            context.Message("Checking that linking to OpenSSL works...")
            ret = context.TryLink(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf.AddTest("CheckLinkSSL", CheckLinkSSL)

        if not conf.CheckLinkSSL():
            conf.env.ConfError("SSL is enabled, but is unavailable")

        env.SetConfigHeaderDefine("MONGO_CONFIG_SSL")
        env.Append( MONGO_CRYPTO=["openssl"] )

        if conf.CheckDeclaration(
            "FIPS_mode_set",
            includes="""
                #include <openssl/crypto.h>
                #include <openssl/evp.h>
            """):
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_FIPS_MODE_SET')

    else:
        env.Append( MONGO_CRYPTO=["tom"] )

    if use_system_version_of_library("pcre"):
        conf.FindSysLibDep("pcre", ["pcre"])
        conf.FindSysLibDep("pcrecpp", ["pcrecpp"])
    else:
        env.Prepend(CPPDEFINES=['PCRE_STATIC'])

    if use_system_version_of_library("snappy"):
        conf.FindSysLibDep("snappy", ["snappy"])

    if use_system_version_of_library("zlib"):
        conf.FindSysLibDep("zlib", ["zdll" if conf.env.TargetOSIs('windows') else "z"])

    if use_system_version_of_library("stemmer"):
        conf.FindSysLibDep("stemmer", ["stemmer"])

    if use_system_version_of_library("yaml"):
        conf.FindSysLibDep("yaml", ["yaml-cpp"])

    if use_system_version_of_library("intel_decimal128"):
        conf.FindSysLibDep("intel_decimal128", ["bid"])

    if use_system_version_of_library("icu"):
        conf.FindSysLibDep("icudata", ["icudata"])
        # We can't use FindSysLibDep() for icui18n and icuuc below, since SConf.CheckLib() (which
        # FindSysLibDep() relies on) doesn't expose an 'extra_libs' parameter to indicate that the
        # library being tested has additional dependencies (icuuc depends on icudata, and icui18n
        # depends on both). As a workaround, we skip the configure check for these two libraries and
        # manually assign the library name. We hope that if the user has icudata installed on their
        # system, then they also have icu18n and icuuc installed.
        conf.env['LIBDEPS_ICUI18N_SYSLIBDEP'] = 'icui18n'
        conf.env['LIBDEPS_ICUUC_SYSLIBDEP'] = 'icuuc'

    if wiredtiger and use_system_version_of_library("wiredtiger"):
        if not conf.CheckCXXHeader( "wiredtiger.h" ):
            myenv.ConfError("Cannot find wiredtiger headers")
        conf.FindSysLibDep("wiredtiger", ["wiredtiger"])

    conf.env.Append(
        CPPDEFINES=[
            ("BOOST_THREAD_VERSION", "4"),
            # Boost thread v4's variadic thread support doesn't
            # permit more than four parameters.
            "BOOST_THREAD_DONT_PROVIDE_VARIADIC_THREAD",
            "BOOST_SYSTEM_NO_DEPRECATED",
        ]
    )

    if use_system_version_of_library("boost"):
        if not conf.CheckCXXHeader( "boost/filesystem/operations.hpp" ):
            myenv.ConfError("can't find boost headers")
        if not conf.CheckBoostMinVersion():
            myenv.ConfError("system's version of boost is too old. version 1.49 or better required")

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
    else:
        # For the built in boost, we can set these without risking ODR violations, so do so.
        conf.env.Append(
            CPPDEFINES=[
                # We don't want interruptions because we don't use
                # them and they have a performance cost.
                "BOOST_THREAD_DONT_PROVIDE_INTERRUPTIONS",

                # We believe that none of our platforms are affected
                # by the EINTR bug. Setting this avoids a retry loop
                # in boosts mutex.hpp that we don't want to pay for.
                "BOOST_THREAD_HAS_NO_EINTR_BUG",
            ],
        )

    if posix_system:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_HEADER_UNISTD_H")
        conf.CheckLib('rt')
        conf.CheckLib('dl')

    if posix_monotonic_clock:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK")

    if (conf.CheckCXXHeader( "execinfo.h" ) and
        conf.CheckDeclaration('backtrace', includes='#include <execinfo.h>') and
        conf.CheckDeclaration('backtrace_symbols', includes='#include <execinfo.h>') and
        conf.CheckDeclaration('backtrace_symbols_fd', includes='#include <execinfo.h>')):

        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE")

    conf.env["_HAVEPCAP"] = conf.CheckLib( ["pcap", "wpcap"], autoadd=False )

    if env.TargetOSIs('solaris'):
        conf.CheckLib( "nsl" )

    conf.env['MONGO_BUILD_SASL_CLIENT'] = bool(has_option("use-sasl-client"))
    if conf.env['MONGO_BUILD_SASL_CLIENT'] and not conf.CheckLibWithHeader(
            "sasl2", 
            ["stddef.h","sasl/sasl.h"], 
            "C", 
            "sasl_version_info(0, 0, 0, 0, 0, 0);", 
            autoadd=False ):
        myenv.ConfError("Couldn't find SASL header/libraries")

    # requires ports devel/libexecinfo to be installed
    if env.TargetOSIs('freebsd', 'openbsd'):
        if not conf.CheckLib("execinfo"):
            myenv.ConfError("Cannot find libexecinfo, please install devel/libexecinfo.")

    # 'tcmalloc' needs to be the last library linked. Please, add new libraries before this 
    # point.
    if myenv['MONGO_ALLOCATOR'] == 'tcmalloc':
        if use_system_version_of_library('tcmalloc'):
            conf.FindSysLibDep("tcmalloc", ["tcmalloc"])
    elif myenv['MONGO_ALLOCATOR'] == 'system':
        pass
    else:
        myenv.FatalError("Invalid --allocator parameter: $MONGO_ALLOCATOR")

    def CheckStdAtomic(context, base_type, extra_message):
        test_body = """
        #include <atomic>

        int main() {{
            std::atomic<{0}> x;

            x.store(0);
            {0} y = 1;
            x.fetch_add(y);
            x.fetch_sub(y);
            x.exchange(y);
            x.compare_exchange_strong(y, x);
            return x.load();
        }}
        """.format(base_type)

        context.Message(
            "Checking if std::atomic<{0}> works{1}... ".format(
                base_type, extra_message
            )
        )

        ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret
    conf.AddTest("CheckStdAtomic", CheckStdAtomic)

    def check_all_atomics(extra_message=''):
        for t in ('int64_t', 'uint64_t', 'int32_t', 'uint32_t'):
            if not conf.CheckStdAtomic(t, extra_message):
                return False
        return True

    if not check_all_atomics():
        if not conf.CheckLib('atomic', symbol=None, header=None, language='C', autoadd=1):
            myenv.ConfError("Some atomic ops are not intrinsically supported, but "
                "no libatomic found")
        if not check_all_atomics(' with libatomic'):
            myenv.ConfError("The toolchain does not support std::atomic, cannot continue")

    # ask each module to configure itself and the build environment.
    moduleconfig.configure_modules(mongo_modules, conf)

    return conf.Finish()

env = doConfigure( env )

# Load the compilation_db tool. We want to do this after configure so we don't end up with
# compilation database entries for the configure tests, which is weird.
env.Tool("compilation_db")

def checkErrorCodes():
    import buildscripts.errorcodes as x
    if x.checkErrorCodes() == False:
        env.FatalError("next id to use: {0}", x.getNextCode())

checkErrorCodes()

# --- lint ----

def doLint( env , target , source ):
    import buildscripts.eslint
    if not buildscripts.eslint.lint(None, dirmode=True, glob=["jstests/", "src/mongo/"]):
        raise Exception("ESLint errors")

    import buildscripts.clang_format
    if not buildscripts.clang_format.lint_all(None):
        raise Exception("clang-format lint errors")

    import buildscripts.lint
    if not buildscripts.lint.run_lint( [ "src/mongo/" ] ):
        raise Exception( "lint errors" )

env.Alias( "lint" , [] , [ doLint ] )
env.AlwaysBuild( "lint" )


#  ----  INSTALL -------

def getSystemInstallName():
    arch_name = env.subst('$MONGO_DISTARCH')

    # We need to make sure the directory names inside dist tarballs are permanently
    # consistent, even if the target OS name used in scons is different. Any differences
    # between the names used by env.TargetOSIs/env.GetTargetOSName should be added
    # to the translation dictionary below.
    os_name_translations = {
        'windows': 'win32'
    }
    os_name = env.GetTargetOSName()
    os_name = os_name_translations.get(os_name, os_name)
    n = os_name + "-" + arch_name

    if len(mongo_modules):
            n += "-" + "-".join(m.name for m in mongo_modules)

    dn = env.subst('$MONGO_DISTMOD')
    if len(dn) > 0:
        n = n + "-" + dn

    return n

# This function will add the version.txt file to the source tarball
# so that versioning will work without having the git repo available.
def add_version_to_distsrc(env, archive):
    version_file_path = env.subst("$MONGO_DIST_SRC_PREFIX") + "version.json"
    if version_file_path not in archive:
        version_data = {
            'version': env['MONGO_VERSION'],
            'githash': env['MONGO_GIT_HASH'],
        }
        archive.append_file_contents(
            version_file_path,
            json.dumps(
                version_data,
                sort_keys=True,
                indent=4,
                separators=(',', ': ')
            )
        )

env.AddDistSrcCallback(add_version_to_distsrc)

env['SERVER_DIST_BASENAME'] = env.subst('mongodb-%s-$MONGO_DISTNAME' % (getSystemInstallName()))

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
Export("usemozjs")
Export('module_sconscripts')
Export("debugBuild optBuild")
Export("wiredtiger")
Export("mmapv1")
Export("endian")

def injectMongoIncludePaths(thisEnv):
    thisEnv.AppendUnique(CPPPATH=['$BUILD_DIR'])
env.AddMethod(injectMongoIncludePaths, 'InjectMongoIncludePaths')

compileDb = env.Alias("compiledb", env.CompilationDatabase('compile_commands.json'))

env.Alias("distsrc-tar", env.DistSrc("mongodb-src-${MONGO_VERSION}.tar"))
env.Alias("distsrc-tgz", env.GZip(
    target="mongodb-src-${MONGO_VERSION}.tgz",
    source=["mongodb-src-${MONGO_VERSION}.tar"])
)
env.Alias("distsrc-zip", env.DistSrc("mongodb-src-${MONGO_VERSION}.zip"))
env.Alias("distsrc", "distsrc-tgz")

env.SConscript('src/SConscript', variant_dir='$BUILD_DIR', duplicate=False)

env.Alias('all', ['core', 'tools', 'dbtest', 'unittests', 'integration_tests'])

# Substitute environment variables in any build targets so that we can
# say, for instance:
#
# > scons --prefix=/foo/bar '$INSTALL_DIR'
# or
# > scons \$BUILD_DIR/mongo/base
#
# That way, you can reference targets under the variant dir or install
# path via an invariant name.
#
# We need to replace the values in the BUILD_TARGETS object in-place
# because SCons wants it to be a particular object.
for i, s in enumerate(BUILD_TARGETS):
    BUILD_TARGETS[i] = env.subst(s)
