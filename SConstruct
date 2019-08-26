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
import subprocess
import sys
import textwrap
import uuid

import SCons

# This must be first, even before EnsureSConsVersion, if
# we are to avoid bulk loading all tools in the DefaultEnvironment.
DefaultEnvironment(tools=[])

# These come from site_scons/mongo. Import these things
# after calling DefaultEnvironment, for the sake of paranoia.
import mongo
import mongo.platform as mongo_platform
import mongo.toolchain as mongo_toolchain
import mongo.generators as mongo_generators

EnsurePythonVersion(3, 5)
EnsureSConsVersion(3, 1, 1)

from buildscripts import utils
from buildscripts import moduleconfig

import libdeps
import psutil

scons_invocation = '{} {}'.format(sys.executable, ' '.join(sys.argv))
print('scons: running with args {}'.format(scons_invocation))

atexit.register(mongo.print_build_failures)

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


# Always randomize the build order to shake out missing edges, and to help the cache:
# http://scons.org/doc/production/HTML/scons-user/ch24s06.html
SetOption('random', 1)

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

add_option('install-mode',
    choices=['legacy', 'hygienic'],
    default='legacy',
    help='select type of installation',
    nargs=1,
    type='choice',
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
    help='Enable or Disable SSL',
    choices=['on', 'off'],
    default='on',
    const='on',
    nargs='?',
    type='choice',
)

add_option('ssl-provider',
    choices=['auto', 'openssl', 'native'],
    default='auto',
    help='Select the SSL engine to use',
    nargs=1,
    type='choice',
)

add_option('mmapv1',
    choices=['auto', 'on', 'off'],
    const='on',
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

add_option('mobile-se',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Enable Mobile Storage Engine',
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

add_option('separate-debug',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Produce separate debug files (only effective in --install-mode=hygienic)',
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
    choices=['on', 'size', 'off'],
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
    choices=["auto", "system", "tcmalloc", "tcmalloc-experimental"],
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

add_option('enable-free-mon',
    choices=["auto", "on", "off"],
    default="auto",
    help='Disable support for Free Monitoring to avoid HTTP client library dependencies',
    type='choice',
)

add_option('enable-http-client',
    choices=["auto", "on", "off"],
    default="auto",
    help='Enable support for HTTP client requests (required WinHTTP or cURL)',
    type='choice',
)

add_option('use-sasl-client',
    help='Support SASL authentication in the client library',
    nargs=0,
)

add_option('use-system-tcmalloc',
    help='use system version of tcmalloc library',
    nargs=0,
)

add_option('use-system-fmt',
    help='use system version of fmt library',
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

add_option('use-diagnostic-latches',
    choices=['on', 'off'],
    default='on',
    help='Enable annotated Mutex types',
    type='choice',
)


add_option('system-boost-lib-search-suffixes',
    help='Comma delimited sequence of boost library suffixes to search',
)

add_option('use-system-abseil-cpp',
    help='use system version of abseil-cpp libraries',
    nargs=0,
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

add_option('use-system-google-benchmark',
    help='use system version of Google benchmark library',
    nargs=0,
)

add_option('use-system-zlib',
    help='use system version of zlib library',
    nargs=0,
)

add_option('use-system-zstd',
    help="use system version of Zstandard library",
    nargs=0,
)

add_option('use-system-sqlite',
    help='use system version of sqlite library',
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

add_option('use-system-mongo-c',
    choices=['on', 'off', 'auto'],
    const='on',
    default="auto",
    help="use system version of the mongo-c-driver (auto will use it if it's found)",
    nargs='?',
    type='choice',
)

add_option('use-system-kms-message',
    help='use system version of kms-message library',
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

add_option('build-mongoreplay',
    help='when building with --use-new-tools, build mongoreplay ( requires pcap dev )',
    nargs=1,
)

add_option('use-cpu-profiler',
    help='Link against the google-perftools profiler library',
    nargs=0,
)

add_option('build-fast-and-loose',
    choices=['on', 'off', 'auto'],
    const='on',
    default='auto',
    help='looser dependency checking',
    nargs='?',
    type='choice',
)

add_option('disable-warnings-as-errors',
    help="Don't add -Werror to compiler command line",
    nargs=0,
)

add_option('detect-odr-violations',
    help="Have the linker try to detect ODR violations, if supported",
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
    'win7'    : ('0601', '0000'),
    'ws08r2'  : ('0601', '0000'),
    'win8'    : ('0602', '0000'),
    'win81'   : ('0603', '0000'),
}

add_option('win-version-min',
    choices=list(win_version_min_choices.keys()),
    default=None,
    help='minimum Windows version to support',
    type='choice',
)

add_option('cache',
    choices=["all", "nolinked"],
    const='all',
    help='Use an object cache rather than a per-build variant directory (experimental)',
    nargs='?',
)

add_option('cache-dir',
    default='$BUILD_ROOT/scons/cache',
    help='Specify the directory to use for caching objects if --cache is in use',
)

add_option("cxx-std",
    choices=["17"],
    default="17",
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

link_model_choices = ['auto', 'object', 'static', 'dynamic', 'dynamic-strict', 'dynamic-sdk']
add_option('link-model',
    choices=link_model_choices,
    default='auto',
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

add_option('use-hardware-crc32',
    choices=["on", "off"],
    default="on",
    help="Enable CRC32 hardware accelaration",
    type='choice',
)

add_option('git-decider',
    choices=["on", "off"],
    const='on',
    default="off",
    help="Use git metadata for out-of-date detection for source files",
    nargs='?',
    type="choice",
)

add_option('toolchain-root',
    default=None,
    help="Names a toolchain root for use with toolchain selection Variables files in etc/scons",
)

add_option('msvc-debugging-format',
    choices=["codeview", "pdb"],
    default="codeview",
    help='Debugging format in debug builds using msvc. Codeview (/Z7) or Program database (/Zi). Default is codeview.',
    type='choice',
)

add_option('jlink',
        help="Limit link concurrency. Takes either an integer to limit to or a"
        " float between 0 and 1.0 whereby jobs will be multiplied to get the final"
        " jlink value."
        "\n\nExample: --jlink=0.75 --jobs 8 will result in a jlink value of 6",
        const=0.5,
        default=None,
        nargs='?',
        type=float)

try:
    with open("version.json", "r") as version_fp:
        version_data = json.load(version_fp)

    if 'version' not in version_data:
        print("version.json does not contain a version string")
        Exit(1)
    if 'githash' not in version_data:
        version_data['githash'] = utils.get_git_version()

except IOError as e:
    # If the file error wasn't because the file is missing, error out
    if e.errno != errno.ENOENT:
        print(("Error opening version.json: {0}".format(e.strerror)))
        Exit(1)

    version_data = {
        'version': utils.get_git_describe()[1:],
        'githash': utils.get_git_version(),
    }

except ValueError as e:
    print(("Error decoding version.json: {0}".format(e)))
    Exit(1)

# Setup the command-line variables
def variable_shlex_converter(val):
    # If the argument is something other than a string, propogate
    # it literally.
    if not isinstance(val, str):
        return val
    parse_mode = get_option('variable-parse-mode')
    if parse_mode == 'auto':
        parse_mode = 'other' if mongo_platform.is_running_os('windows') else 'posix'
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
    if mongo_platform.is_running_os('windows'):
        # we only support MS toolchain on windows
        return ['msvc', 'mslink', 'mslib', 'masm', 'vcredist']
    elif mongo_platform.is_running_os('linux', 'solaris'):
        return ['gcc', 'g++', 'gnulink', 'ar', 'gas']
    elif mongo_platform.is_running_os('darwin'):
        return ['gcc', 'g++', 'applelink', 'ar', 'libtool', 'as', 'xcode']
    else:
        return ["default"]

def variable_tools_converter(val):
    tool_list = shlex.split(val)
    return tool_list + [
        "distsrc",
        "gziptool",
        'idl_tool',
        "jsheader",
        "mongo_benchmark",
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
    print(("Using variable customization file %s" % file))

env_vars = Variables(
    files=variables_files,
    args=ARGUMENTS
)

sconsflags = os.environ.get('SCONSFLAGS', None)
if sconsflags:
    print(("Using SCONSFLAGS environment variable arguments: %s" % sconsflags))

env_vars.Add('ABIDW',
    help="Configures the path to the 'abidw' (a libabigail) utility")

env_vars.Add('AR',
    help='Sets path for the archiver')

env_vars.Add('ARFLAGS',
    help='Sets flags for the archiver',
    converter=variable_shlex_converter)

env_vars.Add(
    'CACHE_SIZE',
    help='Maximum size of the cache (in gigabytes)',
    default=32,
    converter=lambda x:int(x)
)

env_vars.Add(
    'CACHE_PRUNE_TARGET',
    help='Maximum percent in-use in cache after pruning',
    default=66,
    converter=lambda x:int(x)
)

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

env_vars.Add('FRAMEWORKPATH',
    help='Adds paths to the linker search path for darwin frameworks',
    converter=variable_shlex_converter)

env_vars.Add('FRAMEWORKS',
    help='Adds extra darwin frameworks to link against',
    converter=variable_shlex_converter)

env_vars.Add('HOST_ARCH',
    help='Sets the native architecture of the compiler',
    converter=variable_arch_converter,
    default=None)

env_vars.Add('ICECC',
    help='Tell SCons where icecream icecc tool is')

env_vars.Add('ICERUN',
    help='Tell SCons where icecream icerun tool is')

env_vars.Add('ICECC_CREATE_ENV',
    help='Tell SCons where icecc-create-env tool is',
    default='buildscripts/icecc_create_env')

env_vars.Add('ICECC_SCHEDULER',
    help='Tell ICECC where the sceduler daemon is running')

env_vars.Add('ICECC_VERSION',
    help='Tell ICECC where the compiler package is')

env_vars.Add('ICECC_VERSION_ARCH',
    help='Tell ICECC the target archicture for the compiler package, if non-native')

env_vars.Add('LIBPATH',
    help='Adds paths to the linker search path',
    converter=variable_shlex_converter)

env_vars.Add('LIBS',
    help='Adds extra libraries to link against',
    converter=variable_shlex_converter)

env_vars.Add('LINKFLAGS',
    help='Sets flags for the linker',
    converter=variable_shlex_converter)

env_vars.Add('MAXLINELENGTH',
    help='Maximum line length before using temp files',
    # This is very small, but appears to be the least upper bound
    # across our platforms.
    #
    # See https://support.microsoft.com/en-us/help/830473/command-prompt-cmd.-exe-command-line-string-limitation
    default=4095)

# Note: This is only really meaningful when configured via a variables file. See the
# default_buildinfo_environment_data() function for examples of how to use this.
env_vars.Add('MONGO_BUILDINFO_ENVIRONMENT_DATA',
    help='Sets the info returned from the buildInfo command and --version command-line flag',
    default=mongo_generators.default_buildinfo_environment_data())

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

def validate_mongo_version(key, val, env):
    regex = r'^(\d+)\.(\d+)\.(\d+)-?((?:(rc)(\d+))?.*)?'
    if not re.match(regex, val):
        print(("Invalid MONGO_VERSION '{}', or could not derive from version.json or git metadata. Please add a conforming MONGO_VERSION=x.y.z[-extra] as an argument to SCons".format(val)))
        Exit(1)

env_vars.Add('MONGO_VERSION',
    help='Sets the version string for MongoDB',
    default=version_data['version'],
    validator=validate_mongo_version)

env_vars.Add('MONGO_GIT_HASH',
    help='Sets the githash to store in the MongoDB version information',
    default=version_data['githash'])

env_vars.Add('MSVC_USE_SCRIPT',
    help='Sets the script used to setup Visual Studio.')

env_vars.Add('MSVC_VERSION',
    help='Sets the version of Visual C++ to use (e.g. 14.1 for VS2017, 14.2 for VS2019)',
    default="14.1")

env_vars.Add('OBJCOPY',
    help='Sets the path to objcopy',
    default=WhereIs('objcopy'))

# Exposed to be able to cross compile Android/*nix from Windows without ending up with the .exe suffix.
env_vars.Add('PROGSUFFIX',
    help='Sets the suffix for built executable files')

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

env_vars.Add('SHELL',
    help='Pick the shell to use when spawning commands')

env_vars.Add('SHLINKFLAGS',
    help='Sets flags for the linker when building shared libraries',
    converter=variable_shlex_converter)

env_vars.Add('TARGET_ARCH',
    help='Sets the architecture to build for',
    converter=variable_arch_converter,
    default=None)

env_vars.Add('TARGET_OS',
    help='Sets the target OS to build for',
    default=mongo_platform.get_running_os_name())

env_vars.Add('TOOLS',
    help='Sets the list of SCons tools to add to the environment',
    converter=variable_tools_converter,
    default=decide_platform_tools())

env_vars.Add('VARIANT_DIR',
    help='Sets the name (or generator function) for the variant directory',
    default=mongo_generators.default_variant_dir_generator,
)

env_vars.Add('VERBOSE',
    help='Control build verbosity (auto, on/off true/false 1/0)',
    default='auto',
)

env_vars.Add('WINDOWS_OPENSSL_BIN',
    help='Sets the path to the openssl binaries for packaging',
    default='c:/openssl/bin')

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
SConsignFile(str(sconsDataDir.File('sconsign.py3')))

def printLocalInfo():
    import sys, SCons
    print(( "scons version: " + SCons.__version__ ))
    print(( "python version: " + " ".join( [ repr(i) for i in sys.version_info ] ) ))

printLocalInfo()

boostLibs = [ "filesystem", "program_options", "system", "iostreams" ]

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
    ( "on",  "size"  ) : ( True,  True ),
    ( "off", "size"  ) : ( False, True ),
}
debugBuild, optBuild = dbg_opt_mapping[(get_option('dbg'), get_option('opt'))]
optBuildForSize = True if optBuild and get_option('opt') == "size" else False

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
               MODULE_INJECTORS=dict(),
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
               BENCHMARK_ALIAS='benchmarks',
               BENCHMARK_LIST='$BUILD_ROOT/benchmarks.txt',
               CONFIGUREDIR='$BUILD_ROOT/scons/$VARIANT_DIR/sconf_temp',
               CONFIGURELOG='$BUILD_ROOT/scons/config.log',
               INSTALL_DIR=installDir,
               CONFIG_HEADER_DEFINES={},
               LIBDEPS_TAG_EXPANSIONS=[],
               )

env = Environment(variables=env_vars, **envDict)
del envDict

for var in ['CC', 'CXX']:
    if var not in env:
        continue
    path = env[var]
    print('{} is {}'.format(var, path))
    if not os.path.isabs(path):
        which = shutil.which(path)
        if which is None:
            print('{} was not found in $PATH'.format(path))
        else:
            print('{} found in $PATH at {}'.format(path, which))
            path = which

    realpath = os.path.realpath(path)
    if realpath != path:
        print('{} resolves to {}'.format(path, realpath))

env.AddMethod(mongo_platform.env_os_is_wrapper, 'TargetOSIs')
env.AddMethod(mongo_platform.env_get_os_name_wrapper, 'GetTargetOSName')

def fatal_error(env, msg, *args):
    print((msg.format(*args)))
    Exit(1)

def conf_error(env, msg, *args):
    print((msg.format(*args)))
    print(("See {0} for details".format(env.File('$CONFIGURELOG').abspath)))
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
    print((env_vars.GenerateHelpText(env)))
    Exit(0)

unknown_vars = env_vars.UnknownVariables()
if unknown_vars:
    env.FatalError("Unknown variables specified: {0}", ", ".join(list(unknown_vars.keys())))

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
    'arm'        : { 'endian': 'little', 'defines': ('__arm__',) },
    'aarch64'    : { 'endian': 'little', 'defines': ('__arm64__', '__aarch64__')},
    'i386'       : { 'endian': 'little', 'defines': ('__i386', '_M_IX86')},
    'ppc64le'    : { 'endian': 'little', 'defines': ('__powerpc64__',)},
    's390x'      : { 'endian': 'big',    'defines': ('__s390x__',)},
    'sparc'      : { 'endian': 'big',    'defines': ('__sparc',)},
    'x86_64'     : { 'endian': 'little', 'defines': ('__x86_64', '_M_AMD64')},
    'emscripten' : { 'endian': 'little', 'defines': ('__EMSCRIPTEN__', )},
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

    for k in list(processor_macros.keys()):
        ret = run_compile_check(k)
        if ret:
            context.Result('Detected a %s processor' % k)
            return k

    context.Result('Could not detect processor model/architecture')
    return False

# Taken from http://nadeausoftware.com/articles/2012/01/c_c_tip_how_use_compiler_predefined_macros_detect_operating_system
os_macros = {
    "windows": "defined(_WIN32)",
    "solaris": "defined(__sun)",
    "freebsd": "defined(__FreeBSD__)",
    "openbsd": "defined(__OpenBSD__)",
    "iOS": "defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_SIMULATOR",
    "iOS-sim": "defined(__APPLE__) && TARGET_OS_IOS && TARGET_OS_SIMULATOR",
    "tvOS": "defined(__APPLE__) && TARGET_OS_TV && !TARGET_OS_SIMULATOR",
    "tvOS-sim": "defined(__APPLE__) && TARGET_OS_TV && TARGET_OS_SIMULATOR",
    "watchOS": "defined(__APPLE__) && TARGET_OS_WATCH && !TARGET_OS_SIMULATOR",
    "watchOS-sim": "defined(__APPLE__) && TARGET_OS_WATCH && TARGET_OS_SIMULATOR",

    # NOTE: Once we have XCode 8 required, we can rely on the value of TARGET_OS_OSX. In case
    # we are on an older XCode, use TARGET_OS_MAC and TARGET_OS_IPHONE. We don't need to correct
    # the above declarations since we will never target them with anything other than XCode 8.
    "macOS": "defined(__APPLE__) && (TARGET_OS_OSX || (TARGET_OS_MAC && !TARGET_OS_IPHONE))",
    "linux": "defined(__linux__)",
    "android": "defined(__ANDROID__)",
    "emscripten": "defined(__EMSCRIPTEN__)",
}

def CheckForOS(context, which_os):
    test_body = """
    #if defined(__APPLE__)
    #include <TargetConditionals.h>
    #endif
    #if {0}
    /* detected {1} */
    #else
    #error
    #endif
    """.format(os_macros[which_os], which_os)
    context.Message('Checking if target OS {0} is supported by the toolchain... '.format(which_os))
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
if mongo_platform.is_running_os('windows'):
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
    print(("No special config for [{0}] which probably means it won't work".format(env['TARGET_OS'])))
elif not detectConf.CheckForOS(env['TARGET_OS']):
    env.ConfError("TARGET_OS ({0}) is not supported by compiler", env['TARGET_OS'])

detectConf.Finish()

env['CC_VERSION'] = mongo_toolchain.get_toolchain_ver(env, 'CC')
env['CXX_VERSION'] = mongo_toolchain.get_toolchain_ver(env, 'CXX')

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
    # using an allocator besides system on android would require either fixing or disabling
    # gperftools on android
    if env.TargetOSIs('windows') or \
       env.TargetOSIs('linux') and not env.TargetOSIs('android'):
        env['MONGO_ALLOCATOR'] = "tcmalloc"
    else:
        env['MONGO_ALLOCATOR'] = "system"
else:
    env['MONGO_ALLOCATOR'] = get_option('allocator')

if has_option("cache"):
    if has_option("gcov"):
        env.FatalError("Mixing --cache and --gcov doesn't work correctly yet. See SERVER-11084")
    env.CacheDir(str(env.Dir(cacheDir)))

# Normalize the link model. If it is auto, then for now both developer and release builds
# use the "static" mode. Somday later, we probably want to make the developer build default
# dynamic, but that will require the hygienic builds project.
link_model = get_option('link-model')
if link_model == "auto":
    link_model = "static"

# Windows can't currently support anything other than 'object' or 'static', until
# we have both hygienic builds and have annotated functions for export.
if env.TargetOSIs('windows') and link_model not in ['object', 'static', 'dynamic-sdk']:
    env.FatalError("Windows builds must use the 'object', 'dynamic-sdk', or 'static' link models")


# The mongodbtoolchain currently doesn't produce working binaries if
# you combine a dynamic build with a non-system allocator, but the
# failure mode is non-obvious. For now, prevent people from wandering
# inadvertantly into this trap. Remove this constraint when
# https://jira.mongodb.org/browse/SERVER-27675 is resolved.
if (link_model == 'dynamic') and ('mongodbtoolchain' in env['CXX']) and (env['MONGO_ALLOCATOR'] != 'system'):
    env.FatalError('Cannot combine the MongoDB toolchain, a dynamic build, and a non-system allocator. Choose two.')

# The 'object' mode for libdeps is enabled by setting _LIBDEPS to $_LIBDEPS_OBJS. The other two
# modes operate in library mode, enabled by setting _LIBDEPS to $_LIBDEPS_LIBS.
env['_LIBDEPS'] = '$_LIBDEPS_OBJS' if link_model == "object" else '$_LIBDEPS_LIBS'

env['BUILDERS']['ProgramObject'] = env['BUILDERS']['StaticObject']
env['BUILDERS']['LibraryObject'] = env['BUILDERS']['StaticObject']

env['SHARPREFIX'] = '$LIBPREFIX'
env['SHARSUFFIX'] = '${SHLIBSUFFIX}${LIBSUFFIX}'
env['BUILDERS']['SharedArchive'] = SCons.Builder.Builder(
    action=env['BUILDERS']['StaticLibrary'].action,
    emitter='$SHAREMITTER',
    prefix='$SHARPREFIX',
    suffix='$SHARSUFFIX',
    src_suffix=env['BUILDERS']['SharedLibrary'].src_suffix,
)

if link_model.startswith("dynamic"):

    def library(env, target, source, *args, **kwargs):
        sharedLibrary = env.SharedLibrary(target, source, *args, **kwargs)
        sharedArchive = env.SharedArchive(target, source=sharedLibrary[0].sources, *args, **kwargs)
        sharedLibrary.extend(sharedArchive)
        return sharedLibrary

    env['BUILDERS']['Library'] = library
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
    # NOTE: The `illegal_cyclic_or_unresolved_dependencies_whitelisted`
    # tag can be applied to a library to indicate that it does not (or
    # cannot) completely express all of its required link dependencies.
    # This can occur for four reasons:
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
    # - The symbol is provided by a third-party library, outside of our
    #   control.
    #
    # All of these are defects in the linking model. In an effort to
    # eliminate these issues, we have begun tagging those libraries
    # that are affected, and requiring that all non-tagged libraries
    # correctly express all dependencies. As we repair each defective
    # library, we can remove the tag. When all the tags are removed
    # the graph will be acyclic. Libraries which are incomplete for the
    # final reason, "libraries outside of our control", may remain for
    # reasons beyond our control. Such libraries ideally should
    # have no dependencies (and thus be leaves in our linking DAG).
    # If that condition is met, then the graph will be acyclic.

    if env.TargetOSIs('darwin'):
        if link_model.startswith('dynamic'):
            print(("WARNING: Building MongoDB server with dynamic linking " +
                  "on macOS is not supported. Static linking is recommended."))

        if link_model == "dynamic-strict":
            # Darwin is strict by default
            pass
        else:
            def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                # On darwin, since it is strict by default, we need to add a flag
                # when libraries are tagged incomplete.
                if ('illegal_cyclic_or_unresolved_dependencies_whitelisted'
                    in target[0].get_env().get("LIBDEPS_TAGS", [])):
                    return ["-Wl,-undefined,dynamic_lookup"]
                return []
            env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)
    elif env.TargetOSIs('windows'):
        if link_model == "dynamic-strict":
            # Windows is strict by default
            pass
        else:
            def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                # On windows, since it is strict by default, we need to add a flag
                # when libraries are tagged incomplete.
                if ('illegal_cyclic_or_unresolved_dependencies_whitelisted'
                    in target[0].get_env().get("LIBDEPS_TAGS", [])):
                    return ["/FORCE:UNRESOLVED"]
                return []
            env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)
    else:
        env.AppendUnique(LINKFLAGS=["-Wl,--no-as-needed"])

        # Using zdefs doesn't work at all with the sanitizers
        if not has_option('sanitize'):

            if link_model == "dynamic-strict":
                env.AppendUnique(SHLINKFLAGS=["-Wl,-z,defs"])
            else:
                # On BFD/gold linker environments, which are not strict by
                # default, we need to add a flag when libraries are not
                # tagged incomplete.
                def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                    if ('illegal_cyclic_or_unresolved_dependencies_whitelisted'
                        not in target[0].get_env().get("LIBDEPS_TAGS", [])):
                        return ["-Wl,-z,defs"]
                    return []
                env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)

if optBuild:
    env.SetConfigHeaderDefine("MONGO_CONFIG_OPTIMIZED_BUILD")

# Enable the fast decider if exlicltly requested or if in 'auto' mode and not in conflict with other
# options.
if get_option('build-fast-and-loose') == 'on' or \
   (get_option('build-fast-and-loose') == 'auto' and \
    not has_option('release') and \
    not has_option('cache')):
    # See http://www.scons.org/wiki/GoFastButton for details
    env.Decider('MD5-timestamp')
    env.SetOption('max_drift', 1)

# If the user has requested the git decider, enable it if it is available. We want to do this after
# we set the basic decider above, so that we 'chain' to that one.
if get_option('git-decider') == 'on':
    git_decider = Tool('git_decider')
    if git_decider.exists(env):
        git_decider(env)

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

# Link tools other than mslink don't setup TEMPFILE in LINKCOM,
# disabling SCons automatically falling back to a temp file when
# running link commands that are over MAXLINELENGTH. With our object
# file linking mode, we frequently hit even the large linux command
# line length, so we want it everywhere. If we aren't using mslink,
# add TEMPFILE in. For verbose builds when using a tempfile, we need
# some trickery so that we print the command we are running, and not
# just the invocation of the compiler being fed the command file.
if not 'mslink' in env['TOOLS']:
    if env.Verbose():
        env["LINKCOM"] = "${{TEMPFILE('{0}', '')}}".format(env['LINKCOM'])
        env["SHLINKCOM"] = "${{TEMPFILE('{0}', '')}}".format(env['SHLINKCOM'])
        if not 'libtool' in env['TOOLS']:
            env["ARCOM"] = "${{TEMPFILE('{0}', '')}}".format(env['ARCOM'])
    else:
        env["LINKCOM"] = "${{TEMPFILE('{0}', 'LINKCOMSTR')}}".format(env['LINKCOM'])
        env["SHLINKCOM"] = "${{TEMPFILE('{0}', 'SHLINKCOMSTR')}}".format(env['SHLINKCOM'])
        if not 'libtool' in env['TOOLS']:
            env["ARCOM"] = "${{TEMPFILE('{0}', 'ARCOMSTR')}}".format(env['ARCOM'])

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

# Both the abidw tool and the thin archive tool must be loaded after
# libdeps, so that the scanners they inject can see the library
# dependencies added by libdeps.
if link_model.startswith("dynamic"):
    # Add in the abi linking tool if the user requested and it is
    # supported on this platform.
    if env.get('ABIDW'):
        abilink = Tool('abilink')
        if abilink.exists(env):
            abilink(env)

if env['_LIBDEPS'] == '$_LIBDEPS_LIBS':
    # The following platforms probably aren't using the binutils
    # toolchain, or may be using it for the archiver but not the
    # linker, and binutils currently is the olny thing that supports
    # thin archives. Don't even try on those platforms.
    if not env.TargetOSIs('solaris', 'darwin', 'windows', 'openbsd'):
        env.Tool('thin_archive')

if env.TargetOSIs('linux', 'freebsd', 'openbsd'):
    env['LINK_LIBGROUP_START'] = '-Wl,--start-group'
    env['LINK_LIBGROUP_END'] = '-Wl,--end-group'
    # NOTE: The leading and trailing spaces here are important. Do not remove them.
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,--whole-archive '
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ' -Wl,--no-whole-archive'
elif env.TargetOSIs('darwin'):
    env['LINK_LIBGROUP_START'] = ''
    env['LINK_LIBGROUP_END'] = ''
    # NOTE: The trailing space here is important. Do not remove it.
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,-force_load '
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ''
elif env.TargetOSIs('solaris'):
    env['LINK_LIBGROUP_START'] = '-Wl,-z,rescan-start'
    env['LINK_LIBGROUP_END'] = '-Wl,-z,rescan-end'
    # NOTE: The leading and trailing spaces here are important. Do not remove them.
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,-z,allextract '
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ' -Wl,-z,defaultextract'
elif env.TargetOSIs('windows'):
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '/WHOLEARCHIVE:'
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ''

# ---- other build setup -----
if debugBuild:
    env.SetConfigHeaderDefine("MONGO_CONFIG_DEBUG_BUILD")
else:
    env.AppendUnique( CPPDEFINES=[ 'NDEBUG' ] )

if env.TargetOSIs('linux'):
    env.Append( LIBS=["m"] )
    if not env.TargetOSIs('android'):
        env.Append( LIBS=["resolv"] )

elif env.TargetOSIs('solaris'):
     env.Append( LIBS=["socket","resolv","lgrp"] )

elif env.TargetOSIs('freebsd'):
    env.Append( LIBS=[ "kvm" ] )
    env.Append( CCFLAGS=[ "-fno-omit-frame-pointer" ] )

elif env.TargetOSIs('darwin'):
    env.Append( LIBS=["resolv"] )

elif env.TargetOSIs('openbsd'):
    env.Append( LIBS=[ "kvm" ] )

elif env.TargetOSIs('windows'):
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

    # Temporary fixes to allow compilation with VS2017
    env.Append(CPPDEFINES=[
        "_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING",
        "_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",
    ])

    # /EHsc exception handling style for visual studio
    # /W3 warning level
    env.Append(CCFLAGS=["/EHsc","/W3"])

    # Suppress some warnings we don't like, or find necessary to
    # suppress. Please keep this list alphabetized and commented.
    env.Append(CCFLAGS=[

        # C4068: unknown pragma. added so that we can specify unknown
        # pragmas for other compilers.
        "/wd4068",

        # C4244: 'conversion' conversion from 'type1' to 'type2',
        # possible loss of data. An integer type is converted to a
        # smaller integer type.
        "/wd4244",

        # C4267: 'var' : conversion from 'size_t' to 'type', possible
        # loss of data. When compiling with /Wp64, or when compiling
        # on a 64-bit operating system, type is 32 bits but size_t is
        # 64 bits when compiling for 64-bit targets. To fix this
        # warning, use size_t instead of a type.
        "/wd4267",

        # C4290: C++ exception specification ignored except to
        # indicate a function is not __declspec(nothrow). A function
        # is declared using exception specification, which Visual C++
        # accepts but does not implement.
        "/wd4290",

        # C4351: On extremely old versions of MSVC (pre 2k5), default
        # constructing an array member in a constructor's
        # initialization list would not zero the array members "in
        # some cases". Since we don't target MSVC versions that old,
        # this warning is safe to ignore.
        "/wd4351",

        # C4355: 'this' : used in base member initializer list. The
        # this pointer is valid only within nonstatic member
        # functions. It cannot be used in the initializer list for a
        # base class
        "/wd4355",

        # C4373: Older versions of MSVC would fail to make a function
        # in a derived class override a virtual function in the
        # parent, when defined inline and at least one of the
        # parameters is made const. The behavior is incorrect under
        # the standard. MSVC is fixed now, and the warning exists
        # merely to alert users who may have relied upon the older,
        # non-compliant behavior. Our code should not have any
        # problems with the older behavior, so we can just disable
        # this warning.
        "/wd4373",

        # C4800: 'type' : forcing value to bool 'true' or 'false'
        # (performance warning). This warning is generated when a
        # value that is not bool is assigned or coerced into type
        # bool.
        "/wd4800",

        # C5041: out-of-line definition for constexpr static data
        # member is not needed and is deprecated in C++17. We still
        # have these, but we don't want to fix them up before we roll
        # over to C++17.
        "/wd5041",
    ])

    # mozjs-60 requires the following
    #  'declaration' : no matching operator delete found; memory will not be freed if
    #  initialization throws an exception
    env.Append( CCFLAGS=["/wd4291"] )

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

    # Warnings as errors
    if not has_option("disable-warnings-as-errors"):
        env.Append( CCFLAGS=["/WX"] )

    env.Append( CPPDEFINES=["_CONSOLE","_CRT_SECURE_NO_WARNINGS", "_SCL_SECURE_NO_WARNINGS"] )

    # this would be for pre-compiled headers, could play with it later
    #env.Append( CCFLAGS=['/Yu"pch.h"'] )

    # Don't send error reports in case of internal compiler error
    env.Append( CCFLAGS= ["/errorReport:none"] )

    # Select debugging format. /Zi gives faster links but seem to use more memory
    if get_option('msvc-debugging-format') == "codeview":
        env['CCPDBFLAGS'] = "/Z7"
    elif get_option('msvc-debugging-format') == "pdb":
        env['CCPDBFLAGS'] = '/Zi /Fd${TARGET}.pdb'

    # /DEBUG will tell the linker to create a .pdb file
    # which WinDbg and Visual Studio will use to resolve
    # symbols if you want to debug a release-mode image.
    # Note that this means we can't do parallel links in the build.
    #
    # Please also note that this has nothing to do with _DEBUG or optimization.
    env.Append( LINKFLAGS=["/DEBUG"] )

    # /MD:  use the multithreaded, DLL version of the run-time library (MSVCRT.lib/MSVCR###.DLL)
    # /MDd: Defines _DEBUG, _MT, _DLL, and uses MSVCRTD.lib/MSVCRD###.DLL

    env.Append(CCFLAGS=["/MDd" if debugBuild else "/MD"])

    if optBuild:
        # /O1:  optimize for size
        # /O2:  optimize for speed (as opposed to size)
        # /Oy-: disable frame pointer optimization (overrides /O2, only affects 32-bit)
        # /INCREMENTAL: NO - disable incremental link - avoid the level of indirection for function
        # calls

        optStr = "/O2" if not optBuildForSize else "/O1"
        env.Append( CCFLAGS=[optStr, "/Oy-"] )
        env.Append( LINKFLAGS=["/INCREMENTAL:NO"])
    else:
        env.Append( CCFLAGS=["/Od"] )

    if debugBuild and not optBuild:
        # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used
        # without having been initialized (implies /Od: no optimizations)
        env.Append( CCFLAGS=["/RTC1"] )

    # Support large object files since some unit-test sources contain a lot of code
    env.Append( CCFLAGS=["/bigobj"] )

    # Set Source and Executable character sets to UTF-8, this will produce a warning C4828 if the
    # file contains invalid UTF-8.
    env.Append( CCFLAGS=["/utf-8" ])

    # Specify standards conformance mode to the compiler.
    env.Append( CCFLAGS=["/permissive-"] )

    # Enables the __cplusplus preprocessor macro to report an updated value for recent C++ language
    # standards support.
    env.Append( CCFLAGS=["/Zc:__cplusplus"] )

    # Tells the compiler to preferentially call global operator delete or operator delete[]
    # functions that have a second parameter of type size_t when the size of the object is available.
    env.Append( CCFLAGS=["/Zc:sizedDealloc"] )

    # Treat volatile according to the ISO standard and do not guarantee acquire/release semantics.
    env.Append( CCFLAGS=["/volatile:iso"] )

    # Tell CL to produce more useful error messages.
    env.Append( CCFLAGS=["/diagnostics:caret"] )

    # This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds.
    env.Append( LINKFLAGS=["/LARGEADDRESSAWARE"] )

    env.Append(
        LIBS=[
            'DbgHelp.lib',
            'Iphlpapi.lib',
            'Psapi.lib',
            'advapi32.lib',
            'bcrypt.lib',
            'crypt32.lib',
            'dnsapi.lib',
            'kernel32.lib',
            'shell32.lib',
            'pdh.lib',
            'version.lib',
            'winmm.lib',
            'ws2_32.lib',
            'secur32.lib',
        ],
    )

# When building on visual studio, this sets the name of the debug symbols file
if env.ToolchainIs('msvc'):
    env['PDB'] = '${TARGET.base}.pdb'

if env.TargetOSIs('posix'):

    # On linux, C code compiled with gcc/clang -std=c11 causes
    # __STRICT_ANSI__ to be set, and that drops out all of the feature
    # test definitions, resulting in confusing errors when we run C
    # language configure checks and expect to be able to find newer
    # POSIX things. Explicitly enabling _XOPEN_SOURCE fixes that, and
    # should be mostly harmless as on Linux, these macros are
    # cumulative. The C++ compiler already sets _XOPEN_SOURCE, and,
    # notably, setting it again does not disable any other feature
    # test macros, so this is safe to do. Other platforms like macOS
    # and BSD have crazy rules, so don't try this there.
    #
    # Furthermore, as both C++ compilers appears to unconditioanlly
    # define _GNU_SOURCE (because libstdc++ requires it), it seems
    # prudent to explicitly add that too, so that C language checks
    # see a consistent set of definitions.
    if env.TargetOSIs('linux'):
        env.AppendUnique(
            CPPDEFINES=[
                ('_XOPEN_SOURCE', 700),
                '_GNU_SOURCE',
            ],
        )

    # Everything on OS X is position independent by default. Solaris doesn't support PIE.
    if not env.TargetOSIs('darwin', 'solaris'):
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
                         "-ggdb" if not env.TargetOSIs('emscripten') else "-g",
                         "-pthread",
                         "-Wall",
                         "-Wsign-compare",
                         "-Wno-unknown-pragmas",
                         "-Winvalid-pch"] )
    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    if env.TargetOSIs('linux', 'darwin', 'solaris'):
        if not has_option("disable-warnings-as-errors"):
            env.Append( CCFLAGS=["-Werror"] )

    env.Append( CXXFLAGS=["-Woverloaded-virtual"] )
    if env.ToolchainIs('clang'):
        env.Append( CXXFLAGS=['-Werror=unused-result'] )

    # On OS X, clang doesn't want the pthread flag at link time, or it
    # issues warnings which make it impossible for us to declare link
    # warnings as errors. See http://stackoverflow.com/a/19382663.
    if not (env.TargetOSIs('darwin') and env.ToolchainIs('clang')):
        env.Append( LINKFLAGS=["-pthread"] )

    # SERVER-9761: Ensure early detection of missing symbols in dependent libraries at program
    # startup.
    if env.TargetOSIs('darwin'):
        if env.TargetOSIs('macOS'):
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

    # Python uses APPDATA to determine the location of user installed
    # site-packages. If we do not pass this variable down to Python
    # subprocesses then anything installed with `pip install --user`
    # will be inaccessible leading to import errors.
    if env.TargetOSIs('windows'):
        appdata = os.getenv('APPDATA', None)
        if appdata is not None:
            env['ENV']['APPDATA'] = appdata

    if env.TargetOSIs('linux') and has_option( "gcov" ):
        env.Append( CCFLAGS=["-fprofile-arcs", "-ftest-coverage", "-fprofile-update=single"] )
        env.Append( LINKFLAGS=["-fprofile-arcs", "-ftest-coverage", "-fprofile-update=single"] )

    if optBuild and not optBuildForSize:
        env.Append( CCFLAGS=["-O2"] )
    elif optBuild and optBuildForSize:
        env.Append( CCFLAGS=["-Os"] )
    else:
        env.Append( CCFLAGS=["-O0"] )

    # Promote linker warnings into errors. We can't yet do this on OS X because its linker considers
    # noall_load obsolete and warns about it.
    if not has_option("disable-warnings-as-errors"):
        env.Append(
            LINKFLAGS=[
                '-Wl,-fatal_warnings' if env.TargetOSIs('darwin') else "-Wl,--fatal-warnings",
            ]
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

mobile_se = False
if get_option('mobile-se') == 'on':
    mobile_se = True

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
    for keysuffix in [ "1" , "2", "ForRollover" ]:
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

# --- check system ---
ssl_provider = None
free_monitoring = get_option("enable-free-mon")
http_client = get_option("enable-http-client")

def doConfigure(myenv):
    global wiredtiger
    global ssl_provider
    global free_monitoring
    global http_client

    # Check that the compilers work.
    #
    # TODO: Currently, we have some flags already injected. Eventually, this should test the
    # bare compilers, and we should re-check at the very end that TryCompile and TryLink still
    # work with the flags we have selected.
    if myenv.ToolchainIs('msvc'):
        compiler_minimum_string = "Microsoft Visual Studio 2017 15.9"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(_MSC_VER)
        #error
        #endif

        #if _MSC_VER < 1916
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('gcc'):
        compiler_minimum_string = "GCC 8.2"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(__GNUC__) || defined(__clang__)
        #error
        #endif

        #if (__GNUC__ < 8) || (__GNUC__ == 8 && __GNUC_MINOR__ < 2)
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('clang'):
        compiler_minimum_string = "clang 7.0 (or Apple XCode 10.0)"
        compiler_test_body = textwrap.dedent(
        """
        #if !defined(__clang__)
        #error
        #endif

        #if defined(__apple_build_version__)
        #if __apple_build_version__ < 10010046
        #error %s or newer is required to build MongoDB
        #endif
        #elif (__clang_major__ < 7) || (__clang_major__ == 7 && __clang_minor__ < 0)
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
            win_version_min = 'ws08r2'

        env['WIN_VERSION_MIN'] = win_version_min
        win_version_min = win_version_min_choices[win_version_min]
        env.Append( CPPDEFINES=[("_WIN32_WINNT", "0x" + win_version_min[0])] )
        env.Append( CPPDEFINES=[("NTDDI_VERSION", "0x" + win_version_min[0] + win_version_min[1])] )

    conf.Finish()

    # We require macOS 10.12 or newer
    if env.TargetOSIs('darwin'):

        # TODO: Better error messages, mention the various -mX-version-min-flags in the error, and
        # single source of truth for versions, plumbed through #ifdef ladder and error messages.
        def CheckDarwinMinima(context):
            test_body = """
            #include <Availability.h>
            #include <AvailabilityMacros.h>
            #include <TargetConditionals.h>

            #if TARGET_OS_OSX && (__MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_12)
            #error 1
            #endif
            """

            context.Message("Checking for sufficient {0} target version minimum... ".format(context.env['TARGET_OS']))
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(myenv, help=False, custom_tests={
            "CheckDarwinMinima" : CheckDarwinMinima,
        })

        if not conf.CheckDarwinMinima():
            conf.env.ConfError("Required target minimum of macOS 10.12 not found")

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
            for kw in list(test_mutation.keys()):
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
                'CheckFlag' : lambda ctx : CheckFlagTest(ctx, tool, extension, flag)
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

        # As of clang in Android NDK 17, these warnings appears in boost and/or ICU, and get escalated to errors
        AddToCCFLAGSIfSupported(myenv, "-Wno-tautological-constant-compare")
        AddToCCFLAGSIfSupported(myenv, "-Wno-tautological-unsigned-zero-compare")
        AddToCCFLAGSIfSupported(myenv, "-Wno-tautological-unsigned-enum-zero-compare")

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

        # Disable warning about templates that can't be implicitly instantiated. It is an attempt to
        # make a link error into an easier-to-debug compiler failure, but it triggers false
        # positives if explicit instantiation is used in a TU that can see the full definition. This
        # is a problem at least for the S2 headers.
        AddToCXXFLAGSIfSupported(myenv, "-Wno-undefined-var-template")

        # This warning was added in clang-4.0, but it warns about code that is required on some
        # platforms. Since the warning just states that 'explicit instantiation of [a template] that
        # occurs after an explicit specialization has no effect', it is harmless on platforms where
        # it isn't required
        AddToCXXFLAGSIfSupported(myenv, "-Wno-instantiation-after-specialization")

        # This warning was added in clang-5 and flags many of our lambdas. Since it isn't actively
        # harmful to capture unused variables we are suppressing for now with a plan to fix later.
        AddToCCFLAGSIfSupported(myenv, "-Wno-unused-lambda-capture")

        # This warning was added in clang-5 and incorrectly flags our implementation of
        # exceptionToStatus(). See https://bugs.llvm.org/show_bug.cgi?id=34804
        AddToCCFLAGSIfSupported(myenv, "-Wno-exceptions")

        # Enable sized deallocation support.
        AddToCXXFLAGSIfSupported(myenv, '-fsized-deallocation')

        # This warning was added in Apple clang version 11 and flags many explicitly defaulted move
        # constructors and assignment operators for being implicitly deleted, which is not useful.
        AddToCXXFLAGSIfSupported(myenv, "-Wno-defaulted-function-deleted")

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

        # As of XCode 9, this flag must be present (it is not enabled
        # by -Wall), in order to enforce that -mXXX-version-min=YYY
        # will enforce that you don't use APIs from ZZZ.
        if env.TargetOSIs('darwin'):
            AddToCCFLAGSIfSupported(env, '-Wunguarded-availability')

    if get_option('runtime-hardening') == "on":
        # Enable 'strong' stack protection preferentially, but fall back to 'all' if it is not
        # available. Note that we need to add these to the LINKFLAGS as well, since otherwise we
        # might not link libssp when we need to (see SERVER-12456).
        if myenv.ToolchainIs('gcc', 'clang'):
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

        if myenv.ToolchainIs('clang'):
            # TODO: There are several interesting things to try here, but they each have
            # consequences we need to investigate.
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

    if has_option('osx-version-min'):
        message="""
        The --osx-version-min option is no longer supported.

        To specify a target minimum for Darwin platforms, please explicitly add the appropriate options
        to CCFLAGS and LINKFLAGS on the command line:

        macOS: scons CCFLAGS="-mmacosx-version-min=10.11" LINKFLAGS="-mmacosx-version-min=10.11" ..
        iOS  : scons CCFLAGS="-miphoneos-version-min=10.3" LINKFLAGS="-miphoneos-version-min=10.3" ...
        tvOS : scons CCFLAGS="-mtvos-version-min=10.3" LINKFLAGS="-tvos-version-min=10.3" ...

        Note that MongoDB requires macOS 10.10, iOS 10.2, or tvOS 10.2 or later.
        """
        myenv.ConfError(textwrap.dedent(message))

    usingLibStdCxx = False
    if has_option('libc++'):
        if not myenv.ToolchainIs('clang'):
            myenv.FatalError('libc++ is currently only supported for clang')
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

    if myenv.ToolchainIs('msvc'):
        if get_option('cxx-std') == "17":
            myenv.AppendUnique(CCFLAGS=['/std:c++17'])
    else:
        if get_option('cxx-std') == "17":
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++17'):
                myenv.ConfError('Compiler does not honor -std=c++17')

        if not AddToCFLAGSIfSupported(myenv, '-std=c11'):
            myenv.ConfError("C++14/17 mode selected for C++ files, but can't enable C11 for C files")

    if using_system_version_of_cxx_libraries():
        print( 'WARNING: System versions of C++ libraries must be compiled with C++14/17 support' )

    def CheckCxx17(context):
        test_body = """
        #if __cplusplus < 201703L
        #error
        #endif
        namespace NestedNamespaceDecls::AreACXX17Feature {};
        """

        context.Message('Checking for C++17... ')
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckCxx17' : CheckCxx17,
    })

    if get_option('cxx-std') == "17" and not conf.CheckCxx17():
        myenv.ConfError('C++17 support is required to build MongoDB')

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
        # except on 32 bit android where it breaks boost
        if not conf.CheckTypeSize('off_t', includes="#include <sys/types.h>", expect=8):
            if not env.TargetOSIs('android'):
                myenv.Append(CPPDEFINES=["_FILE_OFFSET_BITS=64"])

        conf.Finish()

    if has_option('sanitize'):

        if not myenv.ToolchainIs('clang', 'gcc'):
            env.FatalError('sanitize is only supported with clang or gcc')

        if myenv.ToolchainIs('gcc'):
            # GCC's implementation of ASAN depends on libdl.
            env.Append(LIBS=['dl'])

        sanitizer_list = get_option('sanitize').split(',')

        using_lsan = 'leak' in sanitizer_list
        using_asan = 'address' in sanitizer_list or using_lsan
        using_tsan = 'thread' in sanitizer_list
        using_ubsan = 'undefined' in sanitizer_list

        if env['MONGO_ALLOCATOR'] in ['tcmalloc', 'tcmalloc-experimental'] and (using_lsan or using_asan):
            # There are multiply defined symbols between the sanitizer and
            # our vendorized tcmalloc.
            env.FatalError("Cannot use --sanitize=leak or --sanitize=address with tcmalloc")

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

        # Select those unique black files that are associated with the
        # currently enabled sanitizers, but filter out those that are
        # zero length.
        blackfiles = {v for (k, v) in blackfiles_map.items() if k in sanitizer_list}
        blackfiles = [f for f in blackfiles if os.stat(f.path).st_size != 0]

        # Filter out any blacklist options that the toolchain doesn't support.
        supportedBlackfiles = []
        blackfilesTestEnv = myenv.Clone()
        for blackfile in blackfiles:
            if AddToCCFLAGSIfSupported(blackfilesTestEnv, "-fsanitize-blacklist=%s" % blackfile):
                supportedBlackfiles.append(blackfile)
        blackfilesTestEnv = None
        blackfiles = sorted(supportedBlackfiles)

        # If we ended up with any blackfiles after the above filters,
        # then expand them into compiler flag arguments, and use a
        # generator to return at command line expansion time so that
        # we can change the signature if the file contents change.
        if blackfiles:
            blacklist_options=["-fsanitize-blacklist=%s" % blackfile for blackfile in blackfiles]
            def SanitizerBlacklistGenerator(source, target, env, for_signature):
                if for_signature:
                    return [f.get_csig() for f in blackfiles]
                return blacklist_options
            myenv.AppendUnique(
                SANITIZER_BLACKLIST_GENERATOR=SanitizerBlacklistGenerator,
                CCFLAGS="${SANITIZER_BLACKLIST_GENERATOR}",
                LINKFLAGS="${SANITIZER_BLACKLIST_GENERATOR}",
            )

        llvm_symbolizer = get_option('llvm-symbolizer')
        if os.path.isabs(llvm_symbolizer):
            if not myenv.File(llvm_symbolizer).exists():
                print(("WARNING: Specified symbolizer '%s' not found" % llvm_symbolizer))
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

        if using_asan:
            # Unfortunately, abseil requires that we make these macros
            # (this, and THREAD_ and UNDEFINED_BEHAVIOR_ below) set,
            # because apparently it is too hard to query the running
            # compiler. We do this unconditionally because abseil is
            # basically pervasive via the 'base' library.
            myenv.AppendUnique(CPPDEFINES=['ADDRESS_SANITIZER'])

        if using_tsan:
            tsan_options += "suppressions=\"%s\" " % myenv.File("#etc/tsan.suppressions").abspath
            myenv['ENV']['TSAN_OPTIONS'] = tsan_options
            myenv.AppendUnique(CPPDEFINES=['THREAD_SANITIZER'])

        if using_ubsan:
            # By default, undefined behavior sanitizer doesn't stop on
            # the first error. Make it so. Newer versions of clang
            # have renamed the flag.
            if not AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover"):
                AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover=undefined")
            myenv.AppendUnique(CPPDEFINES=['UNDEFINED_BEHAVIOR_SANITIZER'])

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
        # because it is much faster. Don't use it if the user has already configured another linker
        # selection manually.
        if not any(flag.startswith('-fuse-ld=') for flag in env['LINKFLAGS']):
            AddToLINKFLAGSIfSupported(myenv, '-fuse-ld=gold')

        # Explicitly enable GNU build id's if the linker supports it.
        AddToLINKFLAGSIfSupported(myenv, '-Wl,--build-id')

        # Explicitly use the new gnu hash section if the linker offers
        # it, except on android since older runtimes seem to not
        # support it. For that platform, use 'both'.
        if env.TargetOSIs('android'):
            AddToLINKFLAGSIfSupported(myenv, '-Wl,--hash-style=both')
        else:
            AddToLINKFLAGSIfSupported(myenv, '-Wl,--hash-style=gnu')

        # Try to have the linker tell us about ODR violations. Don't
        # use it when using clang with libstdc++, as libstdc++ was
        # probably built with GCC. That combination appears to cause
        # false positives for the ODR detector. See SERVER-28133 for
        # additional details.
        if (get_option('detect-odr-violations') and
                not (myenv.ToolchainIs('clang') and usingLibStdCxx)):
            AddToLINKFLAGSIfSupported(myenv, '-Wl,--detect-odr-violations')

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

    # Avoid deduping symbols on OS X debug builds, as it takes a long time.
    if not optBuild and myenv.ToolchainIs('clang') and env.TargetOSIs('darwin'):
        AddToLINKFLAGSIfSupported(myenv, "-Wl,-no_deduplicate")

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

            if myenv.TargetOSIs('darwin'):
                AddToLINKFLAGSIfSupported(myenv, '-Wl,-object_path_lto,${TARGET}.lto')

        else:
            myenv.ConfError("Don't know how to enable --lto on current toolchain")

    if get_option('runtime-hardening') == "on" and optBuild:
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

    def CheckThreadLocal(context):
        test_body = """
        thread_local int tsp_int = 1;
        int main(int argc, char** argv) {{
            return !(tsp_int == argc);
        }}
        """
        context.Message('Checking for storage class thread_local ')
        ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(myenv, help=False, custom_tests = {
        'CheckThreadLocal': CheckThreadLocal
    })
    if not conf.CheckThreadLocal():
        env.ConfError("Compiler must support the thread_local storage class")
    conf.Finish()

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

    # pthread_setname_np was added in GLIBC 2.12, and Solaris 11.3
    if posix_system:
        myenv = conf.Finish()

        def CheckPThreadSetNameNP(context):
            compile_test_body = textwrap.dedent("""
            #ifndef _GNU_SOURCE
            #define _GNU_SOURCE
            #endif
            #include <pthread.h>

            int main() {
                pthread_setname_np(pthread_self(), "test");
                return 0;
            }
            """)

            context.Message("Checking if pthread_setname_np is supported... ")
            result = context.TryCompile(compile_test_body, ".cpp")
            context.Result(result)
            return result

        conf = Configure(myenv, custom_tests = {
            'CheckPThreadSetNameNP': CheckPThreadSetNameNP,
        })

        if conf.CheckPThreadSetNameNP():
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP")

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

    ### --ssl and --ssl-provider checks
    def checkOpenSSL(conf):
        sslLibName = "ssl"
        cryptoLibName = "crypto"
        sslLinkDependencies = ["crypto", "dl"]
        if conf.env.TargetOSIs('freebsd'):
            sslLinkDependencies = ["crypto"]

        if conf.env.TargetOSIs('windows'):
            sslLibName = "ssleay32"
            cryptoLibName = "libeay32"
            sslLinkDependencies = ["libeay32"]

        # Used to import system certificate keychains
        if conf.env.TargetOSIs('darwin'):
            conf.env.AppendUnique(FRAMEWORKS=[
                'CoreFoundation',
                'Security',
            ])

        def maybeIssueDarwinSSLAdvice(env):
            if env.TargetOSIs('macOS'):
                advice = textwrap.dedent(
                    """\
                    NOTE: Recent versions of macOS no longer ship headers for the system OpenSSL libraries.
                    NOTE: Either build without the --ssl flag, or describe how to find OpenSSL.
                    NOTE: Set the include path for the OpenSSL headers with the CPPPATH SCons variable.
                    NOTE: Set the library path for OpenSSL libraries with the LIBPATH SCons variable.
                    NOTE: If you are using HomeBrew, and have installed OpenSSL, this might look like:
                    \tscons CPPPATH=/usr/local/opt/openssl/include LIBPATH=/usr/local/opt/openssl/lib ...
                    NOTE: Consult the output of 'brew info openssl' for details on the correct paths."""
                )
                print(advice)
                brew = env.WhereIs('brew')
                if brew:
                    try:
                        # TODO: If we could programmatically extract the paths from the info output
                        # we could give a better message here, but brew info's machine readable output
                        # doesn't seem to include the whole 'caveats' section.
                        message = subprocess.check_output([brew, "info", "openssl"]).decode('utf-8')
                        advice = textwrap.dedent(
                            """\
                            NOTE: HomeBrew installed to {0} appears to have OpenSSL installed.
                            NOTE: Consult the output from '{0} info openssl' to determine CPPPATH and LIBPATH."""
                        ).format(brew, message)

                        print(advice)
                    except:
                        pass

        if not conf.CheckLibWithHeader(
                cryptoLibName,
                ["openssl/crypto.h"],
                "C",
                "SSLeay_version(0);",
                autoadd=True):
            maybeIssueDarwinSSLAdvice(conf.env)
            conf.env.ConfError("Couldn't find OpenSSL crypto.h header and library")

        def CheckLibSSL(context):
            res = SCons.Conftest.CheckLib(context,
                     libs=[sslLibName],
                     extra_libs=sslLinkDependencies,
                     header='#include "openssl/ssl.h"',
                     language="C",
                     call="SSL_version(NULL);",
                     autoadd=True)
            context.did_show_result = 1
            return not res

        conf.AddTest("CheckLibSSL", CheckLibSSL)

        if not conf.CheckLibSSL():
           maybeIssueDarwinSSLAdvice(conf.env)
           conf.env.ConfError("Couldn't find OpenSSL ssl.h header and library")

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
            maybeIssueDarwinSSLAdvice(conf.env)
            conf.env.ConfError("SSL is enabled, but is unavailable")

        if conf.CheckDeclaration(
            "FIPS_mode_set",
            includes="""
                #include <openssl/crypto.h>
                #include <openssl/evp.h>
            """):
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_FIPS_MODE_SET')

        if conf.CheckDeclaration(
            "d2i_ASN1_SEQUENCE_ANY",
            includes="""
                #include <openssl/asn1.h>
            """):
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_ASN1_ANY_DEFINITIONS')

        def CheckOpenSSL_EC_DH(context):
            compile_test_body = textwrap.dedent("""
            #include <openssl/ssl.h>

            int main() {
                SSL_CTX_set_ecdh_auto(0, 0);
                SSL_set_ecdh_auto(0, 0);
                return 0;
            }
            """)

            context.Message("Checking if SSL_[CTX_]_set_ecdh_auto is supported... ")
            result = context.TryCompile(compile_test_body, ".cpp")
            context.Result(result)
            return result

        def CheckOpenSSL_EC_KEY_new(context):
            compile_test_body = textwrap.dedent("""
            #include <openssl/ssl.h>
            #include <openssl/ec.h>

            int main() {
                SSL_CTX_set_tmp_ecdh(0, 0);
                EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
                EC_KEY_free(0);
                return 0;
            }
            """)

            context.Message("Checking if EC_KEY_new_by_curve_name is supported... ")
            result = context.TryCompile(compile_test_body, ".cpp")
            context.Result(result)
            return result

        conf.AddTest("CheckOpenSSL_EC_DH", CheckOpenSSL_EC_DH)
        if conf.CheckOpenSSL_EC_DH():
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_SSL_SET_ECDH_AUTO')

        conf.AddTest("CheckOpenSSL_EC_KEY_new", CheckOpenSSL_EC_KEY_new)
        if conf.CheckOpenSSL_EC_KEY_new():
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_SSL_EC_KEY_NEW')

    ssl_provider = get_option("ssl-provider")
    if ssl_provider == 'auto':
        if conf.env.TargetOSIs('windows', 'darwin', 'macOS'):
            ssl_provider = 'native'
        else:
            ssl_provider = 'openssl'

    if ssl_provider == 'native':
        if conf.env.TargetOSIs('windows'):
            ssl_provider = 'windows'
            env.SetConfigHeaderDefine("MONGO_CONFIG_SSL_PROVIDER", "MONGO_CONFIG_SSL_PROVIDER_WINDOWS")
            conf.env.Append( MONGO_CRYPTO=["windows"] )

        elif conf.env.TargetOSIs('darwin', 'macOS'):
            ssl_provider = 'apple'
            env.SetConfigHeaderDefine("MONGO_CONFIG_SSL_PROVIDER", "MONGO_CONFIG_SSL_PROVIDER_APPLE")
            conf.env.Append( MONGO_CRYPTO=["apple"] )
            conf.env.AppendUnique(FRAMEWORKS=[
                'CoreFoundation',
                'Security',
            ])

    # We require ssl by default unless the user has specified --ssl=off
    require_ssl = get_option("ssl") != "off"

    if ssl_provider == 'openssl':
        if require_ssl:
            checkOpenSSL(conf)
            # Working OpenSSL available, use it.
            env.SetConfigHeaderDefine("MONGO_CONFIG_SSL_PROVIDER", "MONGO_CONFIG_SSL_PROVIDER_OPENSSL")

            conf.env.Append( MONGO_CRYPTO=["openssl"] )
        else:
            # If we don't need an SSL build, we can get by with TomCrypt.
            conf.env.Append( MONGO_CRYPTO=["tom"] )

    if require_ssl:
        # Either crypto engine is native,
        # or it's OpenSSL and has been checked to be working.
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_SSL")
        print(("Using SSL Provider: {0}".format(ssl_provider)))
    else:
        ssl_provider = "none"

    # The Windows build needs the openssl binaries if it targets openssl
    if conf.env.TargetOSIs('windows') and ssl_provider == "openssl":
        # Add the SSL binaries to the zip file distribution
        def addOpenSslLibraryToDistArchive(file_name):
            openssl_bin_path = os.path.normpath(env['WINDOWS_OPENSSL_BIN'].lower())
            full_file_name = os.path.join(openssl_bin_path, file_name)
            if os.path.exists(full_file_name):
                env.Append(ARCHIVE_ADDITIONS=[full_file_name])
                env.Append(ARCHIVE_ADDITION_DIR_MAP={
                        openssl_bin_path: "bin"
                        })
                return True
            else:
                return False

        files = ['ssleay32.dll', 'libeay32.dll']
        for extra_file in files:
            if not addOpenSslLibraryToDistArchive(extra_file):
                print(("WARNING: Cannot find SSL library '%s'" % extra_file))

    def checkHTTPLib(required=False):
        # WinHTTP available on Windows
        if env.TargetOSIs("windows"):
            return True

        # libcurl on all other platforms
        if conf.CheckLibWithHeader(
            "curl",
            ["curl/curl.h"], "C",
            "curl_global_init(0);",
            autoadd=False):
            return True

        if required:
            env.ConfError("Could not find <curl/curl.h> and curl lib")

        return False

    if use_system_version_of_library("pcre"):
        conf.FindSysLibDep("pcre", ["pcre"])
        conf.FindSysLibDep("pcrecpp", ["pcrecpp"])
    else:
        env.Prepend(CPPDEFINES=['PCRE_STATIC'])

    if use_system_version_of_library("snappy"):
        conf.FindSysLibDep("snappy", ["snappy"])

    if use_system_version_of_library("zlib"):
        conf.FindSysLibDep("zlib", ["zdll" if conf.env.TargetOSIs('windows') else "z"])

    if use_system_version_of_library("zstd"):
        conf.FindSysLibDep("zstd", ["libzstd" if conf.env.TargetOSIs('windows') else "zstd"])

    if use_system_version_of_library("stemmer"):
        conf.FindSysLibDep("stemmer", ["stemmer"])

    if use_system_version_of_library("yaml"):
        conf.FindSysLibDep("yaml", ["yaml-cpp"])

    if use_system_version_of_library("fmt"):
        conf.FindSysLibDep("fmt", ["fmt"])

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

    if use_system_version_of_library("sqlite"):
        if not conf.CheckCXXHeader( "sqlite3.h" ):
            myenv.ConfError("Cannot find sqlite headers")
        conf.FindSysLibDep("sqlite", ["sqlite3"])

    conf.env.Append(
        CPPDEFINES=[
            "BOOST_SYSTEM_NO_DEPRECATED",
            "BOOST_MATH_NO_LONG_DOUBLE_MATH_FUNCTIONS",
            "BOOST_ENABLE_ASSERT_DEBUG_HANDLER",
            "ABSL_FORCE_ALIGNED_ACCESS",
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
    if posix_system:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_HEADER_UNISTD_H")
        conf.CheckLib('rt')
        conf.CheckLib('dl')

    if posix_monotonic_clock:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK")

    if get_option('use-diagnostic-latches') == 'off':
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_USE_RAW_LATCHES")

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
    elif myenv['MONGO_ALLOCATOR'] in ['system', 'tcmalloc-experimental']:
        pass
    else:
        myenv.FatalError("Invalid --allocator parameter: $MONGO_ALLOCATOR")

    def CheckStdAtomic(context, base_type, extra_message):
        test_body = """
        #include <atomic>

        int main(int argc, char* argv[]) {{
            std::atomic<{0}> x;

            x.store(0);
            // Use argc to ensure we can't optimize everything away.
            {0} y = argc;
            x.fetch_add(y);
            x.fetch_sub(y);
            x.exchange(y);
            if (x.compare_exchange_strong(y, x) && x.is_lock_free())
                return 0;
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

    def CheckExtendedAlignment(context, size):
        test_body = """
            #include <atomic>
            #include <mutex>
            #include <cstddef>

            static_assert(alignof(std::max_align_t) < {0}, "whatever");

            alignas({0}) std::mutex aligned_mutex;
            alignas({0}) std::atomic<int> aligned_atomic;

            struct alignas({0}) aligned_struct_mutex {{
                std::mutex m;
            }};

            struct alignas({0}) aligned_struct_atomic {{
                std::atomic<int> m;
            }};

            struct holds_aligned_mutexes {{
                alignas({0}) std::mutex m1;
                alignas({0}) std::mutex m2;
            }} hm;

            struct holds_aligned_atomics {{
                alignas({0}) std::atomic<int> a1;
                alignas({0}) std::atomic<int> a2;
            }} ha;
        """.format(size)

        context.Message('Checking for extended alignment {0} for concurrency types... '.format(size))
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf.AddTest('CheckExtendedAlignment', CheckExtendedAlignment)

    # If we don't have a specialized search sequence for this
    # architecture, assume 64 byte cache lines, which is pretty
    # standard. If for some reason the compiler can't offer that, try
    # 32.
    default_alignment_search_sequence = [ 64, 32 ]

    # The following are the target architectures for which we have
    # some knowledge that they have larger cache line sizes. In
    # particular, POWER8 uses 128 byte lines and zSeries uses 256. We
    # start at the goal state, and work down until we find something
    # the compiler can actualy do for us.
    extended_alignment_search_sequence = {
        'ppc64le' : [ 128, 64, 32 ],
        's390x' : [ 256, 128, 64, 32 ],
    }

    for size in extended_alignment_search_sequence.get(env['TARGET_ARCH'], default_alignment_search_sequence):
        if conf.CheckExtendedAlignment(size):
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT", size)
            break

    def CheckMongoCMinVersion(context):
        compile_test_body = textwrap.dedent("""
        #include <mongoc/mongoc.h>

        #if !MONGOC_CHECK_VERSION(1,13,0)
        #error
        #endif
        """)

        context.Message("Checking if mongoc version is 1.13.0 or newer...")
        result = context.TryCompile(compile_test_body, ".cpp")
        context.Result(result)
        return result

    conf.AddTest('CheckMongoCMinVersion', CheckMongoCMinVersion)

    if env.TargetOSIs('darwin'):
        def CheckMongoCFramework(context):
            context.Message("Checking for mongoc_get_major_version() in darwin framework mongoc...")
            test_body = """
            #include <mongoc/mongoc.h>

            int main() {
                mongoc_get_major_version();

                return EXIT_SUCCESS;
            }
            """

            lastFRAMEWORKS = context.env['FRAMEWORKS']
            context.env.Append(FRAMEWORKS=['mongoc'])
            result = context.TryLink(textwrap.dedent(test_body), ".c")
            context.Result(result)
            context.env['FRAMEWORKS'] = lastFRAMEWORKS
            return result

        conf.AddTest('CheckMongoCFramework', CheckMongoCFramework)

    mongoc_mode = get_option('use-system-mongo-c')
    conf.env['MONGO_HAVE_LIBMONGOC'] = False
    if mongoc_mode != 'off':
        if conf.CheckLibWithHeader(
                ["mongoc-1.0"],
                ["mongoc/mongoc.h"],
                "C",
                "mongoc_get_major_version();",
                autoadd=False ):
            conf.env['MONGO_HAVE_LIBMONGOC'] = "library"
        if not conf.env['MONGO_HAVE_LIBMONGOC'] and env.TargetOSIs('darwin') and conf.CheckMongoCFramework():
            conf.env['MONGO_HAVE_LIBMONGOC'] = "framework"
        if not conf.env['MONGO_HAVE_LIBMONGOC'] and mongoc_mode == 'on':
            myenv.ConfError("Failed to find the required C driver headers")
        if conf.env['MONGO_HAVE_LIBMONGOC'] and not conf.CheckMongoCMinVersion():
            myenv.ConfError("Version of mongoc is too old. Version 1.13+ required")

    # ask each module to configure itself and the build environment.
    moduleconfig.configure_modules(mongo_modules, conf)

    # Resolve --enable-free-mon
    if free_monitoring == "auto":
        if 'enterprise' not in env['MONGO_MODULES']:
            free_monitoring = "on"
        else:
            free_monitoring = "off"

    if free_monitoring == "on":
        checkHTTPLib(required=True)

    # Resolve --enable-http-client
    if http_client == "auto":
        if checkHTTPLib():
            http_client = "on"
        else:
            print("Disabling http-client as libcurl was not found")
            http_client = "off"
    elif http_client == "on":
        checkHTTPLib(required=True)

    # Sanity check.
    # We know that http_client was explicitly disabled here,
    # because the free_monitoring check would have failed if no http lib were available.
    if (free_monitoring == "on") and (http_client == "off"):
        env.ConfError("FreeMonitoring requires an HTTP client which has been explicitly disabled")

    if env['TARGET_ARCH'] == "ppc64le":
        # This checks for an altivec optimization we use in full text search.
        # Different versions of gcc appear to put output bytes in different
        # parts of the output vector produced by vec_vbpermq.  This configure
        # check looks to see which format the compiler produces.
        #
        # NOTE: This breaks cross compiles, as it relies on checking runtime functionality for the
        # environment we're in.  A flag to choose the index, or the possibility that we don't have
        # multiple versions to support (after a compiler upgrade) could solve the problem if we
        # eventually need them.
        def CheckAltivecVbpermqOutput(context, index):
            test_body = """
                #include <altivec.h>
                #include <cstring>
                #include <cstdint>
                #include <cstdlib>

                int main() {{
                    using Native = __vector signed char;
                    const size_t size = sizeof(Native);
                    const Native bits = {{ 120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0 }};

                    uint8_t inputBuf[size];
                    std::memset(inputBuf, 0xFF, sizeof(inputBuf));

                    for (size_t offset = 0; offset <= size; offset++) {{
                        Native vec = vec_vsx_ld(0, reinterpret_cast<const Native*>(inputBuf));

                        uint64_t mask = vec_extract(vec_vbpermq(vec, bits), {0});

                        size_t initialZeros = (mask == 0 ? size : __builtin_ctzll(mask));
                        if (initialZeros != offset) {{
			    return 1;
                        }}

                        if (offset < size) {{
                            inputBuf[offset] = 0;  // Add an initial 0 for the next loop.
                        }}
                    }}

		    return 0;
                }}
            """.format(index)

            context.Message('Checking for vec_vbperm output in index {0}... '.format(index))
            ret = context.TryRun(textwrap.dedent(test_body), ".cpp")
            context.Result(ret[0])
            return ret[0]

        conf.AddTest('CheckAltivecVbpermqOutput', CheckAltivecVbpermqOutput)

        outputIndex = next((idx for idx in [0,1] if conf.CheckAltivecVbpermqOutput(idx)), None)
        if outputIndex is not None:
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX", outputIndex)
        else:
            myenv.ConfError("Running on ppc64le, but can't find a correct vec_vbpermq output index.  Compiler or platform not supported")

    return conf.Finish()

env = doConfigure( env )

# TODO: Later, this should live somewhere more graceful.
if get_option('install-mode') == 'hygienic':

    if get_option('separate-debug') == "on":
        env.Tool('separate_debug')

    env.Tool('auto_install_binaries')
    if env['PLATFORM'] == 'posix':
        env.AppendUnique(
            RPATH=[
                env.Literal('\\$$ORIGIN/../lib')
            ],
            LINKFLAGS=[
                # Most systems *require* -z,origin to make origin work, but android
                # blows up at runtime if it finds DF_ORIGIN_1 in DT_FLAGS_1.
                # https://android.googlesource.com/platform/bionic/+/cbc80ba9d839675a0c4891e2ab33f39ba51b04b2/linker/linker.h#68
                # https://android.googlesource.com/platform/bionic/+/cbc80ba9d839675a0c4891e2ab33f39ba51b04b2/libc/include/elf.h#215
                '-Wl,-z,origin' if not env.TargetOSIs('android') else [],
                '-Wl,--enable-new-dtags',
            ],
            SHLINKFLAGS=[
                # -h works for both the sun linker and the gnu linker.
                "-Wl,-h,${TARGET.file}",
            ]
        )
    elif env['PLATFORM'] == 'darwin':
        env.AppendUnique(
            LINKFLAGS=[
                '-Wl,-rpath,@loader_path/../lib',
                '-Wl,-rpath,@loader_path/../Frameworks'
            ],
            SHLINKFLAGS=[
                "-Wl,-install_name,@rpath/${TARGET.file}",
            ],
        )
elif get_option('separate-debug') == "on":
    env.FatalError('Cannot use --separate-debug without --install-mode=hygienic')

# Now that we are done with configure checks, enable icecream, if available.
env.Tool('icecream')

# If the flags in the environment are configured for -gsplit-dwarf,
# inject the necessary emitter.
split_dwarf = Tool('split_dwarf')
if split_dwarf.exists(env):
    split_dwarf(env)

# Load the compilation_db tool. We want to do this after configure so we don't end up with
# compilation database entries for the configure tests, which is weird.
env.Tool("compilation_db")

# If we can, load the dagger tool for build dependency graph introspection.
# Dagger is only supported on Linux and OSX (not Windows or Solaris).
should_dagger = ( mongo_platform.is_running_os('osx') or mongo_platform.is_running_os('linux')  ) and "dagger" in COMMAND_LINE_TARGETS

if should_dagger:
    env.Tool("dagger")

incremental_link = Tool('incremental_link')
if incremental_link.exists(env):
    incremental_link(env)

def checkErrorCodes():
    import buildscripts.errorcodes as x
    if x.check_error_codes() == False:
        env.FatalError("next id to use: {0}", x.get_next_code())

checkErrorCodes()

# Resource Files are Windows specific
def env_windows_resource_file(env, path):
    if env.TargetOSIs('windows'):
        return [ env.RES(path) ]
    else:
        return []

env.AddMethod(env_windows_resource_file, 'WindowsResourceFile')

# --- lint ----

def doLint( env , target , source ):
    import buildscripts.eslint
    if not buildscripts.eslint.lint(None, dirmode=True, glob=["jstests/", "src/mongo/"]):
        raise Exception("ESLint errors")

    import buildscripts.clang_format
    if not buildscripts.clang_format.lint_all(None):
        raise Exception("clang-format lint errors")

    import buildscripts.pylinters
    buildscripts.pylinters.lint_all(None, {}, [])

run_lint = env.Command(
    target="#run_lint",
    source=["buildscripts/lint.py", "src/mongo"],
    action="$PYTHON ${SOURCES[0]} ${SOURCES[1]}",
)

env.Alias( "lint" , [ run_lint ] , [ doLint ] )
env.AlwaysBuild( "lint" )


#  ----  INSTALL -------

def getSystemInstallName():
    arch_name = env.subst('$MONGO_DISTARCH')

    # We need to make sure the directory names inside dist tarballs are permanently
    # consistent, even if the target OS name used in scons is different. Any differences
    # between the names used by env.TargetOSIs/env.GetTargetOSName should be added
    # to the translation dictionary below.
    os_name_translations = {
        'windows': 'win32',
        'macOS': 'macos'
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
Export("get_option")
Export("has_option")
Export("use_system_version_of_library")
Export("serverJs")
Export("usemozjs")
Export('module_sconscripts')
Export("debugBuild optBuild")
Export("wiredtiger")
Export("mmapv1")
Export("mobile_se")
Export("endian")
Export("ssl_provider")
Export("free_monitoring")
Export("http_client")

def injectMongoIncludePaths(thisEnv):
    thisEnv.AppendUnique(CPPPATH=['$BUILD_DIR'])
env.AddMethod(injectMongoIncludePaths, 'InjectMongoIncludePaths')

def injectModule(env, module, **kwargs):
    injector = env['MODULE_INJECTORS'].get(module)
    if injector:
        return injector(env, **kwargs)
    return env
env.AddMethod(injectModule, 'InjectModule')

compileCommands = env.CompilationDatabase('compile_commands.json')
compileDb = env.Alias("compiledb", compileCommands)

# Microsoft Visual Studio Project generation for code browsing
vcxprojFile = env.Command(
    "mongodb.vcxproj",
    compileCommands,
    r"$PYTHON buildscripts\make_vcxproj.py mongodb")
vcxproj = env.Alias("vcxproj", vcxprojFile)

distSrc = env.DistSrc("mongodb-src-${MONGO_VERSION}.tar")
env.NoCache(distSrc)
env.Alias("distsrc-tar", distSrc)

distSrcGzip = env.GZip(
    target="mongodb-src-${MONGO_VERSION}.tgz",
    source=[distSrc])
env.NoCache(distSrcGzip)
env.Alias("distsrc-tgz", distSrcGzip)

distSrcZip = env.DistSrc("mongodb-src-${MONGO_VERSION}.zip")
env.NoCache(distSrcZip)
env.Alias("distsrc-zip", distSrcZip)

env.Alias("distsrc", "distsrc-tgz")

# Defaults for SCons provided flags. SetOption only sets the option to our value
# if the user did not provide it. So for any flag here if it's explicitly passed
# the values below set with SetOption will be overwritten.
#
# Default j to the number of CPUs on the system. Note: in containers this
# reports the number of CPUs for the host system. Perhaps in a future version of
# psutil it will instead report the correct number when in a container.
#
# The presence of the variable ICECC means the icecream tool is
# enabled and so the default j value should scale accordingly. In this
# scenario multiply the cpu count by 8 to set a reasonable default since the
# cluster can handle many more jobs than your local machine but is
# still throttled by your cpu count in the sense that you can only
# handle so many python threads sending out jobs.
#
# psutil.cpu_count returns None when it can't determine the number. This always
# fails on BSD's for example.
if psutil.cpu_count() is not None and 'ICECC' not in env:
    env.SetOption('num_jobs', psutil.cpu_count())
elif psutil.cpu_count() and 'ICECC' in env:
    env.SetOption('num_jobs', 8 * psutil.cpu_count())


# Do this as close to last as possible before reading SConscripts, so
# that any tools that may have injected other things via emitters are included
# among the side effect adornments.
#
# TODO: Move this to a tool.
if has_option('jlink'):
    jlink = get_option('jlink')
    if jlink <= 0:
        env.FatalError("The argument to jlink must be a positive integer or float")
    elif jlink < 1 and jlink > 0:
        jlink = env.GetOption('num_jobs') * jlink
        jlink = round(jlink)
        if jlink < 1.0:
            print("Computed jlink value was less than 1; Defaulting to 1")
            jlink = 1.0

    jlink = int(jlink)
    target_builders = ['Program', 'SharedLibrary', 'LoadableModule']

    # A bound map of stream (as in stream of work) name to side-effect
    # file. Since SCons will not allow tasks with a shared side-effect
    # to execute concurrently, this gives us a way to limit link jobs
    # independently of overall SCons concurrency.
    jlink_stream_map = dict()

    def jlink_emitter(target, source, env):
        name = str(target[0])
        se_name = "#jlink-stream" + str(hash(name) % jlink)
        se_node = jlink_stream_map.get(se_name, None)
        if not se_node:
            se_node = env.Entry(se_name)
            # This may not be necessary, but why chance it
            env.NoCache(se_node)
            jlink_stream_map[se_name] = se_node
        env.SideEffect(se_node, target)
        return (target, source)

    for target_builder in target_builders:
        builder = env['BUILDERS'][target_builder]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([base_emitter, jlink_emitter])
        builder.emitter = new_emitter

# Keep this late in the game so that we can investigate attributes set by all the tools that have run.
if has_option("cache"):
    if get_option("cache") == "nolinked":
        def noCacheEmitter(target, source, env):
            for t in target:
                try:
                    if getattr(t.attributes, 'thin_archive', False):
                        continue
                except(AttributeError):
                    pass
                env.NoCache(t)
            return target, source

        def addNoCacheEmitter(builder):
            origEmitter = builder.emitter
            if SCons.Util.is_Dict(origEmitter):
                for k,v in origEmitter:
                    origEmitter[k] = SCons.Builder.ListEmitter([v, noCacheEmitter])
            elif SCons.Util.is_List(origEmitter):
                origEmitter.append(noCacheEmitter)
            else:
                builder.emitter = SCons.Builder.ListEmitter([origEmitter, noCacheEmitter])

        addNoCacheEmitter(env['BUILDERS']['Program'])
        addNoCacheEmitter(env['BUILDERS']['StaticLibrary'])
        addNoCacheEmitter(env['BUILDERS']['SharedLibrary'])
        addNoCacheEmitter(env['BUILDERS']['LoadableModule'])

env.SConscript(
    dirs=[
        'src',
    ],
    duplicate=False,
    exports=[
        'env',
    ],
    variant_dir='$BUILD_DIR',
)

allTargets = ['core', 'tools', 'unittests', 'integration_tests', 'benchmarks']

if not has_option('noshell') and usemozjs:
    allTargets.extend(['dbtest'])

env.Alias('all', allTargets)

# run the Dagger tool if it's installed
if should_dagger:
    dependencyDb = env.Alias("dagger", env.Dagger('library_dependency_graph.json'))
    # Require everything to be built before trying to extract build dependency information
    env.Requires(dependencyDb, allTargets)

# We don't want installing files to cause them to flow into the cache,
# since presumably we can re-install them from the origin if needed.
env.NoCache(env.FindInstalledFiles())

# Declare the cache prune target
cachePrune = env.Command(
    target="#cache-prune",
    source=[
        "#buildscripts/scons_cache_prune.py",
    ],
    action="$PYTHON ${SOURCES[0]} --cache-dir=${CACHE_DIR.abspath} --cache-size=${CACHE_SIZE} --prune-ratio=${CACHE_PRUNE_TARGET/100.00}",
    CACHE_DIR=env.Dir(cacheDir),
)

env.AlwaysBuild(cachePrune)
env.Alias('cache-prune', cachePrune)

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
