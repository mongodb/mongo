# -*- mode: python; -*-

import atexit
import copy
import errno
import json
import os
import re
import platform
import shlex
import shutil
import stat
import subprocess
import sys
import textwrap
import uuid
from glob import glob

from pkg_resources import parse_version

import SCons
import SCons.Script

# This must be first, even before EnsureSConsVersion, if
# we are to avoid bulk loading all tools in the DefaultEnvironment.
DefaultEnvironment(tools=[])

# These come from site_scons/mongo. Import these things
# after calling DefaultEnvironment, for the sake of paranoia.
import mongo
import mongo.platform as mongo_platform
import mongo.toolchain as mongo_toolchain
import mongo.generators as mongo_generators
import mongo.install_actions as install_actions
from mongo.build_profiles import BUILD_PROFILES

EnsurePythonVersion(3, 6)
EnsureSConsVersion(3, 1, 1)


# Monkey patch SCons.FS.File.release_target_info to be a no-op.
# See https://github.com/SCons/scons/issues/3454
def release_target_info_noop(self):
    pass


SCons.Node.FS.File.release_target_info = release_target_info_noop

from buildscripts import utils
from buildscripts import moduleconfig

import psutil

scons_invocation = '{} {}'.format(sys.executable, ' '.join(sys.argv))
print('scons: running with args {}'.format(scons_invocation))

atexit.register(mongo.print_build_failures)

# An extra instance of the SCons parser is used to manually validate options
# flags. We use it detect some common misspellings/unknown options and
# communicate with the user more effectively than just allowing Configure to
# fail.
# This is to work around issue #4187
# (https://github.com/SCons/scons/issues/4187). Upon a future upgrade to SCons
# that incorporates #4187, we should replace this solution with that.
_parser = SCons.Script.SConsOptions.Parser("")


def add_option(name, **kwargs):
    _parser.add_option('--' + name, **{"default": None, **kwargs})

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

add_option(
    'build-profile',
    choices=list(BUILD_PROFILES.keys()),
    default='default',
    type='choice',
    help='''Short hand for common build options. These profiles are well supported by SDP and are
    kept up to date. Unless you need something specific, it is recommended that you only build with
    these. san is the recommended profile since it exposes bugs before they are found in patch
    builds. Check out site_scons/mongo/build_profiles.py to see each profile.''',
)

build_profile = BUILD_PROFILES[get_option('build-profile')]

add_option(
    'ninja',
    choices=['enabled', 'disabled'],
    default=build_profile.ninja,
    nargs='?',
    const='enabled',
    type='choice',
    help='Enable the build.ninja generator tool stable or canary version',
)

add_option(
    'force-jobs',
    help='Allow more jobs than available cpu\'s when icecream is not enabled.',
    nargs=0,
)

add_option(
    'build-tools',
    choices=['stable', 'next'],
    default='stable',
    type='choice',
    help='Enable experimental build tools',
)

add_option(
    'legacy-tarball',
    choices=['true', 'false'],
    default='false',
    const='true',
    nargs='?',
    type='choice',
    help='Build a tarball matching the old MongoDB dist targets',
)

add_option(
    'lint-scope',
    choices=['all', 'changed'],
    default='all',
    type='choice',
    help='Lint files in the current git diff instead of all files',
)

add_option(
    'install-mode',
    choices=['hygienic'],
    default='hygienic',
    help='select type of installation',
    nargs=1,
    type='choice',
)

add_option(
    'install-action',
    choices=([*install_actions.available_actions] + ['default']),
    default='default',
    help=
    'select mechanism to use to install files (advanced option to reduce disk IO and utilization)',
    nargs=1,
    type='choice',
)

add_option(
    'build-dir',
    default='#build',
    help='build output directory',
)

add_option(
    'release',
    help='release build',
    nargs=0,
)

add_option(
    'lto',
    help='enable link time optimizations (experimental, except with MSVC)',
    nargs=0,
)

add_option(
    'endian',
    choices=['big', 'little', 'auto'],
    default='auto',
    help='endianness of target platform',
    nargs=1,
    type='choice',
)

add_option(
    'disable-minimum-compiler-version-enforcement',
    help='allow use of unsupported older compilers (NEVER for production builds)',
    nargs=0,
)

add_option(
    'ssl',
    help='Enable or Disable SSL',
    choices=['on', 'off'],
    default='on',
    const='on',
    nargs='?',
    type='choice',
)

add_option(
    'wiredtiger',
    choices=['on', 'off'],
    const='on',
    default='on',
    help='Enable wiredtiger',
    nargs='?',
    type='choice',
)

add_option(
    'ocsp-stapling',
    choices=['on', 'off'],
    default='on',
    help='Enable OCSP Stapling on servers',
    nargs='?',
    type='choice',
)

js_engine_choices = ['mozjs', 'none']
add_option(
    'js-engine',
    choices=js_engine_choices,
    default=js_engine_choices[0],
    help='JavaScript scripting engine implementation',
    type='choice',
)

add_option(
    'server-js',
    choices=['on', 'off'],
    default='on',
    help='Build mongod without JavaScript support',
    type='choice',
)

add_option(
    'libc++',
    help='use libc++ (experimental, requires clang)',
    nargs=0,
)

add_option(
    'use-glibcxx-debug',
    help='Enable the glibc++ debug implementations of the C++ standard libary',
    nargs=0,
)

add_option(
    'noshell',
    help="don't build shell",
    nargs=0,
)

add_option(
    'dbg',
    choices=['on', 'off'],
    const='on',
    default=build_profile.dbg,
    help='Enable runtime debugging checks',
    nargs='?',
    type='choice',
)

add_option(
    'disable-ref-track',
    help="Disables runtime tracking of REF state changes for pages within wiredtiger. "
    "Tracking the REF state changes is useful for debugging but there is a small performance cost.",
    nargs=0,
)

add_option(
    'separate-debug',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Produce separate debug files',
    nargs='?',
    type='choice',
)

add_option(
    'spider-monkey-dbg',
    choices=['on', 'off'],
    const='on',
    default='off',
    help='Enable SpiderMonkey debug mode',
    nargs='?',
    type='choice',
)

add_option(
    'opt',
    choices=['on', 'debug', 'size', 'off', 'auto'],
    const='on',
    default=build_profile.opt,
    help='Enable compile-time optimization',
    nargs='?',
    type='choice',
)

experimental_optimizations = [
    'O3',
    'builtin-memcmp',
    'fnsi',
    'nofp',
    'nordyn',
    'sandybridge',
    'tbaa',
    'treevec',
    'vishidden',
]
experimental_optimization_choices = ['*']
experimental_optimization_choices.extend("+" + opt for opt in experimental_optimizations)
experimental_optimization_choices.extend("-" + opt for opt in experimental_optimizations)

add_option(
    'experimental-optimization',
    action="append",
    choices=experimental_optimization_choices,
    const=experimental_optimization_choices[0],
    default=['+sandybridge'],
    help='Enable experimental optimizations',
    nargs='?',
    type='choice',
)

add_option(
    'debug-compress',
    action="append",
    choices=["off", "as", "ld"],
    default=["auto"],
    help="Compress debug sections",
)

add_option(
    'sanitize',
    help='enable selected sanitizers',
    metavar='san1,san2,...sanN',
    default=build_profile.sanitize,
)

add_option(
    'sanitize-coverage',
    help='enable selected coverage sanitizers',
    metavar='cov1,cov2,...covN',
)

add_option(
    'allocator',
    choices=["auto", "system", "tcmalloc", "tcmalloc-experimental"],
    default=build_profile.allocator,
    help='allocator to use (use "auto" for best choice for current platform)',
    type='choice',
)

add_option(
    'gdbserver',
    help='build in gdb server support',
    nargs=0,
)

add_option(
    'lldb-server',
    help='build in lldb server support',
    nargs=0,
)

add_option(
    'gcov',
    help='compile with flags for gcov',
    nargs=0,
)

add_option(
    'enable-free-mon',
    choices=["auto", "on", "off"],
    default="auto",
    help='Disable support for Free Monitoring to avoid HTTP client library dependencies',
    type='choice',
)

add_option(
    'enable-http-client',
    choices=["auto", "on", "off"],
    default="auto",
    help='Enable support for HTTP client requests (required WinHTTP or cURL)',
    type='choice',
)

add_option(
    'use-sasl-client',
    help='Support SASL authentication in the client library',
    nargs=0,
)

add_option(
    'use-diagnostic-latches',
    choices=['on', 'off'],
    default='on',
    help='Enable annotated Mutex types',
    type='choice',
)

# Most of the "use-system-*" options follow a simple form.
for pack in [
    (
        'asio',
        'ASIO',
    ),
    ('boost', ),
    ('fmt', ),
    ('google-benchmark', 'Google benchmark'),
    ('icu', 'ICU'),
    ('intel_decimal128', 'intel decimal128'),
    ('kms-message', ),
    ('pcre2', ),
    ('snappy', ),
    ('stemmer', ),
    ('tcmalloc', ),
    ('libunwind', ),
    ('valgrind', ),
    ('wiredtiger', ),
    ('yaml', ),
    ('zlib', ),
    ('zstd', 'Zstandard'),
]:
    name = pack[0]
    pretty = name
    if len(pack) == 2:
        pretty = pack[1]
    add_option(
        f'use-system-{name}',
        help=f'use system version of {pretty} library',
        nargs=0,
    )

add_option(
    'system-boost-lib-search-suffixes',
    help='Comma delimited sequence of boost library suffixes to search',
)

add_option(
    'use-system-mongo-c',
    choices=['on', 'off', 'auto'],
    const='on',
    default="auto",
    help="use system version of the mongo-c-driver (auto will use it if it's found)",
    nargs='?',
    type='choice',
)

add_option(
    'use-system-all',
    help='use all system libraries',
    nargs=0,
)

add_option(
    'build-fast-and-loose',
    choices=['on', 'off', 'auto'],
    const='on',
    default='auto',
    help='looser dependency checking',
    nargs='?',
    type='choice',
)

add_option(
    "disable-warnings-as-errors",
    action="append",
    choices=["configure", "source"],
    const="source",
    default=[],
    help=
    "Don't add a warnings-as-errors flag to compiler command lines in selected contexts; defaults to 'source' if no argument is provided",
    nargs="?",
    type="choice",
)

add_option(
    'detect-odr-violations',
    help="Have the linker try to detect ODR violations, if supported",
    nargs=0,
)

add_option(
    'variables-help',
    help='Print the help text for SCons variables',
    nargs=0,
)

add_option(
    'osx-version-min',
    help='minimum OS X version to support',
)

# https://docs.microsoft.com/en-us/cpp/porting/modifying-winver-and-win32-winnt?view=vs-2017
# https://docs.microsoft.com/en-us/windows-server/get-started/windows-server-release-info
win_version_min_choices = {
    'win10': ('0A00', '0000'),
    'ws2016': ('0A00', '1607'),
    'ws2019': ('0A00', '1809'),
}

add_option(
    'win-version-min',
    choices=list(win_version_min_choices.keys()),
    default=None,
    help='minimum Windows version to support',
    type='choice',
)

add_option(
    'cache',
    choices=["all", "nolinked"],
    const='all',
    help='Use an object cache rather than a per-build variant directory (experimental)',
    nargs='?',
)

add_option(
    'cache-dir',
    default='$BUILD_ROOT/scons/cache',
    help='Specify the directory to use for caching objects if --cache is in use',
)

add_option(
    'cache-signature-mode',
    choices=['none', 'validate'],
    default="none",
    help='Extra check to validate integrity of cache files after pulling from cache',
)

add_option(
    "cxx-std",
    choices=["17", "20"],
    default="17",
    help="Select the C++ language standard to build with",
)


def find_mongo_custom_variables():
    files = []
    paths = [path for path in sys.path if 'site_scons' in path]
    for path in paths:
        probe = os.path.join(path, 'mongo_custom_variables.py')
        if os.path.isfile(probe):
            files.append(probe)
    return files


add_option(
    'variables-files',
    default=build_profile.variables_files,
    action="append",
    help="Specify variables files to load.",
)

link_model_choices = ['auto', 'object', 'static', 'dynamic', 'dynamic-strict', 'dynamic-sdk']
add_option(
    'link-model',
    choices=link_model_choices,
    default=build_profile.link_model,
    help='Select the linking model for the project',
    type='choice',
)

add_option(
    'linker',
    choices=['auto', 'gold', 'lld', 'bfd'],
    default='auto',
    help='Specify the type of linker to use.',
    type='choice',
)

variable_parse_mode_choices = ['auto', 'posix', 'other']
add_option(
    'variable-parse-mode',
    choices=variable_parse_mode_choices,
    default=variable_parse_mode_choices[0],
    help='Select which parsing mode is used to interpret command line variables',
    type='choice',
)

add_option(
    'modules',
    help="Comma-separated list of modules to build. Empty means none. Default is all.",
)

add_option(
    'runtime-hardening',
    choices=["on", "off"],
    default="on",
    help="Enable runtime hardening features (e.g. stack smash protection)",
    type='choice',
)

experimental_runtime_hardenings = [
    'cfex',
    'controlflow',
    'stackclash',
]
experimental_runtime_hardening_choices = ['*']
experimental_runtime_hardening_choices.extend("+" + opt for opt in experimental_runtime_hardenings)
experimental_runtime_hardening_choices.extend("-" + opt for opt in experimental_runtime_hardenings)

add_option(
    'experimental-runtime-hardening',
    action="append",
    choices=experimental_runtime_hardening_choices,
    const=experimental_runtime_hardening_choices[0],
    default=[],
    help='Enable experimental runtime hardenings',
    nargs='?',
    type='choice',
)

add_option(
    'use-hardware-crc32',
    choices=["on", "off"],
    default="on",
    help="Enable CRC32 hardware acceleration",
    type='choice',
)

add_option(
    'git-decider',
    choices=["on", "off"],
    const='on',
    default="off",
    help="Use git metadata for out-of-date detection for source files",
    nargs='?',
    type="choice",
)

add_option(
    'toolchain-root',
    default=None,
    help="Name a toolchain root for use with toolchain selection Variables files in etc/scons",
)

add_option(
    'msvc-debugging-format',
    choices=["codeview", "pdb"],
    default="codeview",
    help=
    'Debugging format in debug builds using msvc. Codeview (/Z7) or Program database (/Zi). Default is codeview.',
    type='choice',
)

add_option(
    'use-libunwind',
    choices=["on", "off", "auto"],
    const="on",
    default="auto",
    help="Enable libunwind for backtraces",
    nargs="?",
    type='choice',
)

add_option(
    'jlink',
    help="Limit link concurrency. Takes either an integer to limit to or a"
    " float between 0 and 1.0 whereby jobs will be multiplied to get the final"
    " jlink value."
    "\n\nExample: --jlink=0.75 --jobs 8 will result in a jlink value of 6",
    const=0.5,
    default=None,
    nargs='?',
    type=float,
)

add_option(
    'enable-usdt-probes',
    choices=["on", "off", "auto"],
    default="auto",
    help=
    'Enable USDT probes. Default is auto, which is enabled only on Linux with SystemTap headers',
    type='choice',
    nargs='?',
    const='on',
)

add_option(
    'libdeps-debug',
    choices=['on', 'off'],
    const='off',
    help='Print way too much debugging information on how libdeps is handling dependencies.',
    nargs='?',
    type='choice',
)

add_option(
    'libdeps-linting',
    choices=['on', 'off', 'print'],
    const='on',
    default='on',
    help='Enable linting of libdeps. Default is on, optionally \'print\' will not stop the build.',
    nargs='?',
    type='choice',
)

add_option(
    'build-metrics',
    metavar="FILE",
    const='build-metrics.json',
    default='',
    help='Enable tracking of build performance and output data as json.'
    ' Use "-" to output json to stdout, or supply a path to the desired'
    ' file to output to. If no argument is supplied, the default log'
    ' file will be "build-metrics.json".',
    nargs='?',
    type=str,
)

add_option(
    'visibility-support',
    choices=['auto', 'on', 'off'],
    const='auto',
    default='auto',
    help='Enable visibility annotations',
    nargs='?',
    type='choice',
)

add_option(
    'force-macos-dynamic-link',
    default=False,
    action='store_true',
    help='Bypass link-model=dynamic check for macos versions <12.',
)

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


def to_boolean(s):
    if isinstance(s, bool):
        return s
    elif s.lower() in ('1', "on", "true", "yes"):
        return True
    elif s.lower() in ('0', "off", "false", "no"):
        return False
    raise ValueError(f'Invalid value {s}, must be a boolean-like string')


# Setup the command-line variables
def variable_shlex_converter(val):
    # If the argument is something other than a string, propagate
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
        'amd64': 'x86_64',
        'emt64': 'x86_64',
        'x86': 'i386',
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


def split_dwarf_converter(val):
    try:
        return to_boolean(val)
    except ValueError as exc:
        if val.lower() != "auto":
            raise ValueError(
                f'Invalid SPLIT_DWARF value {s}, must be a boolean-like string or "auto"') from exc
    return "auto"


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
    # This list is intentionally not sorted; the order of tool loading
    # matters as some of the tools have dependencies on other tools.
    return tool_list + [
        "distsrc",
        "gziptool",
        "idl_tool",
        "jsheader",
        "mongo_test_execution",
        "mongo_test_list",
        "mongo_benchmark",
        "mongo_integrationtest",
        "mongo_unittest",
        "mongo_libfuzzer",
        "textfile",
    ]


def variable_distsrc_converter(val):
    if not val.endswith("/"):
        return val + "/"
    return val


def fatal_error(env, msg, *args):
    print(msg.format(*args))
    Exit(1)


# Apply the default variables files, and walk the provided
# arguments. Interpret any falsy argument (like the empty string) as
# resetting any prior state. This makes the argument
# --variables-files= destructive of any prior variables files
# arguments, including the default.
variables_files_args = get_option('variables-files')
variables_files = find_mongo_custom_variables()
for variables_file in variables_files_args:
    if variables_file:
        variables_files.append(variables_file)
    else:
        variables_files = []
for vf in variables_files:
    if not os.path.isfile(vf):
        fatal_error(None, f"Specified variables file '{vf}' does not exist")
    print(f"Using variable customization file {vf}")

env_vars = Variables(
    files=variables_files,
    args=ARGUMENTS,
)

sconsflags = os.environ.get('SCONSFLAGS', None)
if sconsflags:
    print(("Using SCONSFLAGS environment variable arguments: %s" % sconsflags))

env_vars.Add(
    'ABIDW',
    help="Configures the path to the 'abidw' (a libabigail) utility",
)

env_vars.Add(
    'AR',
    help='Sets path for the archiver',
)

env_vars.Add(
    'ARFLAGS',
    help='Sets flags for the archiver',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'CCACHE',
    help='Tells SCons where the ccache binary is',
    default=build_profile.CCACHE,
)

env_vars.Add(
    'CACHE_SIZE',
    help='Maximum size of the SCons cache (in gigabytes)',
    default=32,
    converter=lambda x: int(x),
)

env_vars.Add(
    'CACHE_PRUNE_TARGET',
    help='Maximum percent in-use in SCons cache after pruning',
    default=66,
    converter=lambda x: int(x),
)

env_vars.Add(
    'CC',
    help='Selects the C compiler to use',
)

env_vars.Add(
    'CCFLAGS',
    help='Sets flags for the C and C++ compiler',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'ASFLAGS',
    help='Sets assembler specific flags',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'CFLAGS',
    help='Sets flags for the C compiler',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'CPPDEFINES',
    help='Sets pre-processor definitions for C and C++',
    converter=variable_shlex_converter,
    default=[],
)

env_vars.Add(
    'CPPPATH',
    help='Adds paths to the preprocessor search path',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'CXX',
    help='Selects the C++ compiler to use',
)

env_vars.Add(
    'CXXFLAGS',
    help='Sets flags for the C++ compiler',
    converter=variable_shlex_converter,
)

default_destdir = '$BUILD_ROOT/install'
if get_option('ninja') != 'disabled':
    # Workaround for SERVER-53952 where issues wih different
    # ninja files building to the same install dir. Different
    # ninja files need to build to different install dirs.
    default_destdir = '$BUILD_DIR/install'

env_vars.Add(
    'DESTDIR',
    help='Where builds will install files',
    default=default_destdir,
)

env_vars.Add(
    'DSYMUTIL',
    help='Path to the dsymutil utility',
)

env_vars.Add(
    'GITDIFFFLAGS',
    help='Sets flags for git diff',
    default='',
)

env_vars.Add(
    'REVISION',
    help='Base git revision',
    default='',
)

env_vars.Add(
    'ENTERPRISE_REV',
    help='Base git revision of enterprise modules',
    default='',
)

# Note: This probably is only really meaningful when configured via a variables file. It will
# also override whatever the SCons platform defaults would be.
env_vars.Add(
    'ENV',
    help='Sets the environment for subprocesses',
)

env_vars.Add(
    'FRAMEWORKPATH',
    help='Adds paths to the linker search path for darwin frameworks',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'FRAMEWORKS',
    help='Adds extra darwin frameworks to link against',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'HOST_ARCH',
    help='Sets the native architecture of the compiler',
    converter=variable_arch_converter,
    default=None,
)

env_vars.Add(
    'ICECC',
    help='Tells SCons where icecream icecc tool is',
    default=build_profile.ICECC,
)

env_vars.Add(
    'ICERUN',
    help='Tells SCons where icecream icerun tool is',
)

env_vars.Add(
    'ICECC_CREATE_ENV',
    help='Tells SCons where icecc-create-env tool is',
    default='icecc-create-env',
)

env_vars.Add(
    'ICECC_DEBUG',
    help='Tell ICECC to create debug logs (auto, on/off true/false 1/0)',
    default=False,
)

env_vars.Add(
    'ICECC_SCHEDULER',
    help='Tells ICECC where the scheduler daemon is running',
)

env_vars.Add(
    'ICECC_VERSION',
    help='Tells ICECC where the compiler package is',
)

env_vars.Add(
    'ICECC_VERSION_ARCH',
    help='Tells ICECC the target architecture for the compiler package, if non-native',
)

env_vars.Add(
    'LIBPATH',
    help='Adds paths to the linker search path',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'LIBS',
    help='Adds extra libraries to link against',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'LINKFLAGS',
    help='Sets flags for the linker',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'LLVM_SYMBOLIZER',
    help='Name of or path to the LLVM symbolizer',
)

env_vars.Add(
    'MAXLINELENGTH',
    help='Maximum line length before using temp files',
    # This is very small, but appears to be the least upper bound
    # across our platforms.
    #
    # See https://support.microsoft.com/en-us/help/830473/command-prompt-cmd.-exe-command-line-string-limitation
    default=4095,
)

# Note: This is only really meaningful when configured via a variables file. See the
# default_buildinfo_environment_data() function for examples of how to use this.
env_vars.Add(
    'MONGO_BUILDINFO_ENVIRONMENT_DATA',
    help='Sets the info returned from the buildInfo command and --version command-line flag',
    default=mongo_generators.default_buildinfo_environment_data(),
)

env_vars.Add(
    'MONGO_DIST_SRC_PREFIX',
    help='Sets the prefix for files in the source distribution archive',
    converter=variable_distsrc_converter,
    default="mongodb-src-r${MONGO_VERSION}",
)

env_vars.Add(
    'MONGO_DISTARCH',
    help='Adds a string representing the target processor architecture to the dist archive',
    default='$TARGET_ARCH',
)

env_vars.Add(
    'MONGO_DISTMOD',
    help='Adds a string that will be embedded in the dist archive naming',
    default='',
)

env_vars.Add(
    'MONGO_DISTNAME',
    help='Sets the version string to be used in dist archive naming',
    default='$MONGO_VERSION',
)


def validate_mongo_version(key, val, env):
    valid_version_re = re.compile(r'^(\d+)\.(\d+)\.(\d+)-?((?:(rc)(\d+))?.*)?$', re.MULTILINE)
    invalid_version_re = re.compile(r'^0\.0\.0(?:-.*)?', re.MULTILINE)
    if not valid_version_re.match(val) or invalid_version_re.match(val):
        print((
            "Invalid MONGO_VERSION '{}', or could not derive from version.json or git metadata. Please add a conforming MONGO_VERSION=x.y.z[-extra] as an argument to SCons"
            .format(val)))
        Exit(1)


env_vars.Add(
    'MONGO_VERSION',
    help='Sets the version string for MongoDB',
    default=version_data['version'],
    validator=validate_mongo_version,
)

env_vars.Add(
    'MONGO_GIT_HASH',
    help='Sets the githash to store in the MongoDB version information',
    default=version_data['githash'],
)

env_vars.Add(
    'MSVC_USE_SCRIPT',
    help='Sets the script used to setup Visual Studio.',
)

env_vars.Add(
    'MSVC_VERSION',
    help='Sets the version of Visual C++ to use (e.g. 14.2 for VS2019, 14.3 for VS2022)',
    default="14.3",
)

env_vars.Add(
    'NINJA_BUILDDIR',
    help="Location for shared Ninja state",
    default="$BUILD_DIR/ninja",
)

env_vars.Add(
    'NINJA_PREFIX',
    default=build_profile.NINJA_PREFIX,
    help="""A prefix to add to the beginning of generated ninja
files. Useful for when compiling multiple build ninja files for
different configurations, for instance:

    scons --sanitize=asan --ninja NINJA_PREFIX=asan asan.ninja
    scons --sanitize=tsan --ninja NINJA_PREFIX=tsan tsan.ninja

Will generate the files (respectively):

    asan.ninja
    tsan.ninja

Defaults to build. Best used with the --ninja flag so you don't have to
reiterate the prefix in the target name and variable.
""",
)

env_vars.Add(
    'NINJA_SUFFIX', help="""A suffix to add to the end of generated build.ninja
files. Useful for when compiling multiple build ninja files for
different configurations, for instance:

    scons --sanitize=asan --ninja NINJA_SUFFIX=asan build.ninja
    scons --sanitize=tsan --ninja NINJA_SUFFIX=tsan build.ninja

Will generate the files (respectively):

    build.ninja.asan
    build.ninja.tsan
""")

env_vars.Add(
    '__NINJA_NO',
    help="Disables the Ninja tool unconditionally. Not intended for human use.",
    default=0,
)

env_vars.Add(
    'OBJCOPY',
    help='Sets the path to objcopy',
    default=WhereIs('objcopy'),
)

env_vars.Add(
    'PKGDIR',
    help='Directory in which to build packages and archives',
    default='$BUILD_DIR/pkgs',
)

env_vars.Add(
    'PREFIX',
    help='Final installation location of files. Will be made into a sub dir of $DESTDIR',
    default='.',
)

# Exposed to be able to cross compile Android/*nix from Windows without ending up with the .exe suffix.
env_vars.Add(
    'PROGSUFFIX',
    help='Sets the suffix for built executable files',
)

env_vars.Add(
    'RPATH',
    help='Set the RPATH for dynamic libraries and executables',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'SHCCFLAGS',
    help='Sets flags for the C and C++ compiler when building shared libraries',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'SHCFLAGS',
    help='Sets flags for the C compiler when building shared libraries',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'SHCXXFLAGS',
    help='Sets flags for the C++ compiler when building shared libraries',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'SHELL',
    help='Picks the shell to use when spawning commands',
)

env_vars.Add(
    'SHLINKFLAGS',
    help='Sets flags for the linker when building shared libraries',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'SHLINKFLAGS_EXTRA',
    help=
    'Adds additional flags for shared links without overwriting tool configured SHLINKFLAGS values',
    converter=variable_shlex_converter,
)

env_vars.Add(
    'STRIP',
    help='Path to the strip utility (non-darwin platforms probably use OBJCOPY for this)',
)

env_vars.Add(
    'SPLIT_DWARF',
    help='Set the boolean (auto, on/off true/false 1/0) to enable gsplit-dwarf (non-Windows).',
    converter=split_dwarf_converter, default="auto")

env_vars.Add(
    'TAPI',
    help="Configures the path to the 'tapi' (an Xcode) utility",
)

env_vars.Add(
    'TARGET_ARCH',
    help='Sets the architecture to build for',
    converter=variable_arch_converter,
    default=None,
)

env_vars.Add(
    'TARGET_OS',
    help='Sets the target OS to build for',
    default=mongo_platform.get_running_os_name(),
)

env_vars.Add(
    'TOOLS',
    help='Sets the list of SCons tools to add to the environment',
    converter=variable_tools_converter,
    default=decide_platform_tools(),
)

env_vars.Add(
    'VARIANT_DIR',
    help='Sets the name (or generator function) for the variant directory',
    default=build_profile.VARIANT_DIR,
)

env_vars.Add(
    'VERBOSE',
    help='Controls build verbosity (auto, on/off true/false 1/0)',
    default='auto',
)

env_vars.Add(
    'WINDOWS_OPENSSL_BIN',
    help='Sets the path to the openssl binaries for packaging',
    default='c:/openssl/bin',
)

# TODO SERVER-42170 switch to PathIsDirCreate validator
env_vars.Add(
    PathVariable(
        "LOCAL_TMPDIR",
        help='Set the TMPDIR when running tests.',
        default='$BUILD_ROOT/tmp_test_data',
        validator=PathVariable.PathAccept,
    ), )

env_vars.AddVariables(
    ("BUILD_METRICS_EVG_TASK_ID", "Evergreen task ID to add to build metrics data."),
    ("BUILD_METRICS_EVG_BUILD_VARIANT", "Evergreen build variant to add to build metrics data."),
)
for tool in ['build_metrics', 'split_dwarf']:
    try:
        Tool(tool).options(env_vars)
    except ImportError as exc:
        print(f"Failed import while loading options for tool: {tool}\n{exc}")
        pass

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
        Help(env_vars.GenerateHelpText(variables_only_env, sort=True), append=True)
        Help(
            '\nThe \'list-targets\' target can be built to list useful comprehensive build targets\n',
            append=True)
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
# the top level SConstruct, not the invoker's CWD. We could in theory fix this with
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

sconsDataDir = Dir(buildDir).Dir('scons')
SConsignFile(str(sconsDataDir.File('sconsign.py3')))


def printLocalInfo():
    import sys, SCons
    print(("scons version: " + SCons.__version__))
    print(("python version: " + " ".join([repr(i) for i in sys.version_info])))


printLocalInfo()

boostLibs = ["filesystem", "program_options", "system", "iostreams", "thread", "log"]

onlyServer = len(COMMAND_LINE_TARGETS) == 0 or (len(COMMAND_LINE_TARGETS) == 1 and str(
    COMMAND_LINE_TARGETS[0]) in ["mongod", "mongos", "test"])

noshell = has_option("noshell")

jsEngine = get_option("js-engine")

serverJs = get_option("server-js") == "on"

if not serverJs and not jsEngine:
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
envDict = dict(
    BUILD_ROOT=buildDir,
    BUILD_DIR=make_variant_dir_generator(),
    DIST_ARCHIVE_SUFFIX='.tgz',
    MODULE_BANNERS=[],
    MODULE_INJECTORS=dict(),
    PYTHON="$( {} $)".format(sys.executable),
    SERVER_ARCHIVE='${SERVER_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
    UNITTEST_ALIAS='install-unittests',
    # TODO: Move unittests.txt to $BUILD_DIR, but that requires
    # changes to MCI.
    UNITTEST_LIST='$BUILD_ROOT/unittests.txt',
    LIBFUZZER_TEST_ALIAS='install-fuzzertests',
    LIBFUZZER_TEST_LIST='$BUILD_ROOT/libfuzzer_tests.txt',
    INTEGRATION_TEST_ALIAS='install-integration-tests',
    INTEGRATION_TEST_LIST='$BUILD_ROOT/integration_tests.txt',
    BENCHMARK_ALIAS='install-benchmarks',
    BENCHMARK_LIST='$BUILD_ROOT/benchmarks.txt',
    CONFIGUREDIR='$BUILD_ROOT/scons/$VARIANT_DIR/sconf_temp',
    CONFIGURELOG='$BUILD_ROOT/scons/config.log',
    CONFIG_HEADER_DEFINES={},
    LIBDEPS_TAG_EXPANSIONS=[],
)

# By default, we will get the normal SCons tool search. But if the
# user has opted into the next gen tools, add our experimental tool
# directory into the default toolpath, ahead of whatever is already in
# there so it overrides it.
if get_option('build-tools') == 'next':
    SCons.Tool.DefaultToolpath.insert(0, os.path.abspath('site_scons/site_tools/next'))

env = Environment(variables=env_vars, **envDict)
del envDict
env.AddMethod(lambda env, name, **kwargs: add_option(name, **kwargs), 'AddOption')

if get_option('build-metrics'):
    env['BUILD_METRICS_ARTIFACTS_DIR'] = '$BUILD_ROOT/$VARIANT_DIR'
    env.Tool('build_metrics')
    env.AddBuildMetricsMetaData('evg_id', env.get("BUILD_METRICS_EVG_TASK_ID", "UNKNOWN"))
    env.AddBuildMetricsMetaData('variant', env.get("BUILD_METRICS_EVG_BUILD_VARIANT", "UNKNOWN"))

# TODO SERVER-42170 We can remove this Execute call
# when support for PathIsDirCreate can be used as a validator
# to the Variable above.
env.Execute(SCons.Defaults.Mkdir(env.Dir('$LOCAL_TMPDIR')))

if get_option('cache-signature-mode') == 'validate':
    validate_cache_dir = Tool('validate_cache_dir')
    if validate_cache_dir.exists(env):
        validate_cache_dir(env)
    else:
        env.FatalError("Failed to enable validate_cache_dir tool.")

# Only print the spinner if stdout is a tty
if sys.stdout.isatty():
    Progress(['-\r', '\\\r', '|\r', '/\r'], interval=50)


# We are going to start running conf tests soon, so setup
# --disable-warnings-as-errors as soon as possible.
def create_werror_generator(flagname):
    werror_conftests = 'configure' not in get_option('disable-warnings-as-errors')
    werror_source = 'source' not in get_option('disable-warnings-as-errors')

    def generator(target, source, env, for_signature):
        if werror_conftests and "conftest" in str(target[0]):
            return flagname

        if werror_source:
            return flagname

        return str()

    return generator


env.Append(
    CCFLAGS=['$CCFLAGS_GENERATE_WERROR'],
    CCFLAGS_GENERATE_WERROR=create_werror_generator('$CCFLAGS_WERROR'),
    CXXFLAGS=['$CXXFLAGS_GENERATE_WERROR'],
    CXXFLAGS_GENERATE_WERROR=create_werror_generator('$CXXFLAGS_WERROR'),
    LINKFLAGS=['$LINKFLAGS_GENERATE_WERROR'],
    LINKFLAGS_GENERATE_WERROR=create_werror_generator('$LINKFLAGS_WERROR'),
)

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


def conf_error(env, msg, *args):
    print(msg.format(*args))
    print("See {0} for details".format(env.File('$CONFIGURELOG').abspath))
    Exit(1)


env.AddMethod(fatal_error, 'FatalError')
env.AddMethod(conf_error, 'ConfError')

# Normalize the VERBOSE Option, and make its value available as a
# function.
if env['VERBOSE'] == "auto":
    env['VERBOSE'] = not sys.stdout.isatty()
else:
    try:
        env['VERBOSE'] = to_boolean(env['VERBOSE'])
    except ValueError as e:
        env.FatalError(f"Error setting VERBOSE variable: {e}")
env.AddMethod(lambda env: env['VERBOSE'], 'Verbose')

# Normalize the ICECC_DEBUG option
try:
    env['ICECC_DEBUG'] = to_boolean(env['ICECC_DEBUG'])
except ValueError as e:
    env.FatalError("Error setting ICECC_DEBUG variable: {e}")

if has_option('variables-help'):
    print(env_vars.GenerateHelpText(env))
    Exit(0)

unknown_vars = env_vars.UnknownVariables()
if unknown_vars:
    env.FatalError("Unknown variables specified: {0}", ", ".join(list(unknown_vars.keys())))

if get_option('install-action') != 'default' and get_option('ninja') != "disabled":
    env.FatalError("Cannot use non-default install actions when generating Ninja.")
install_actions.setup(env, get_option('install-action'))


def set_config_header_define(env, varname, varval=1):
    env['CONFIG_HEADER_DEFINES'][varname] = varval


env.AddMethod(set_config_header_define, 'SetConfigHeaderDefine')

detectEnv = env.Clone()

# Identify the toolchain in use. We currently support the following:
# These macros came from
# http://nadeausoftware.com/articles/2012/10/c_c_tip_how_detect_compiler_name_and_version_using_compiler_predefined_macros
toolchain_macros = {
    'GCC': 'defined(__GNUC__) && !defined(__clang__)',
    'clang': 'defined(__clang__)',
    'MSVC': 'defined(_MSC_VER)',
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


endian = get_option("endian")

if endian == "auto":
    endian = sys.byteorder

processor_macros = {
    'aarch64': {'endian': 'little', 'check': '(defined(__arm64__) || defined(__aarch64__))'},
    'emscripten': {'endian': 'little', 'check': '(defined(__EMSCRIPTEN__))'},
    'ppc64le': {'endian': 'little', 'check': '(defined(__powerpc64__))'},
    'riscv64': {'endian': 'little', 'check': '(defined(__riscv)) && (__riscv_xlen == 64)'},
    's390x': {'endian': 'big', 'check': '(defined(__s390x__))'},
    'x86_64': {'endian': 'little', 'check': '(defined(__x86_64) || defined(_M_AMD64))'},
}


def CheckForProcessor(context, which_arch):
    def run_compile_check(arch):
        if not endian == processor_macros[arch]['endian']:
            return False

        test_body = """
        #if {0}
        /* Detected {1} */
        #else
        #error not {1}
        #endif
        """.format(processor_macros[arch]['check'], arch)

        return context.TryCompile(textwrap.dedent(test_body), ".c")

    if which_arch:
        ret = run_compile_check(which_arch)
        context.Message('Checking if target processor is %s ' % which_arch)
        context.Result(ret)
        return ret

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


detectSystem = Configure(
    detectEnv,
    help=False,
    custom_tests={
        'CheckForToolchain': CheckForToolchain,
        'CheckForProcessor': CheckForProcessor,
        'CheckForOS': CheckForOS,
    },
)

toolchain_search_sequence = ["GCC", "clang"]
if mongo_platform.is_running_os('windows'):
    toolchain_search_sequence = ['MSVC', 'clang', 'GCC']

detected_toolchain = None
for candidate_toolchain in toolchain_search_sequence:
    if detectSystem.CheckForToolchain(candidate_toolchain, "C++", "CXX", ".cpp"):
        detected_toolchain = candidate_toolchain
        break

if not detected_toolchain:
    env.ConfError("Couldn't identify the C++ compiler")

if not detectSystem.CheckForToolchain(detected_toolchain, "C", "CC", ".c"):
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

releaseBuild = has_option("release")
optBuild = get_option('opt')
debugBuild = get_option('dbg') == "on"

if env.ToolchainIs('clang'):
    # LLVM utilizes the stack extensively without optimization enabled, which
    # causes the built product to easily blow through our 1M stack size whenever
    # either gcov or sanitizers are enabled. Ref: SERVER-65684
    if has_option('gcov') and optBuild not in ("on", "debug"):
        env.FatalError("Error: A clang --gcov build must have either --opt=debug or --opt=on to " +
                       "prevent crashes due to excessive stack usage")

    if has_option('sanitize') and optBuild not in ("on", "debug"):
        env.FatalError("Error: A clang --sanitize build must have either --opt=debug or --opt=on " +
                       "to prevent crashes due to excessive stack usage")

# Special cases - if debug is not enabled and optimization is not specified,
# default to full optimizationm otherwise turn it off.
if optBuild == "auto":
    optBuild = "on" if not debugBuild else "off"

if releaseBuild and (debugBuild or optBuild != "on"):
    env.FatalError(
        "Error: A --release build may not have debugging, and must have full optimization")

if env['TARGET_ARCH']:
    if not detectSystem.CheckForProcessor(env['TARGET_ARCH']):
        env.ConfError("Could not detect processor specified in TARGET_ARCH variable")
else:
    detected_processor = detectSystem.CheckForProcessor(None)
    if not detected_processor:
        env.ConfError("Failed to detect a supported target architecture")
    env['TARGET_ARCH'] = detected_processor

if env['TARGET_OS'] not in os_macros:
    print("No special config for [{0}] which probably means it won't work".format(env['TARGET_OS']))
elif not detectSystem.CheckForOS(env['TARGET_OS']):
    env.ConfError("TARGET_OS ({0}) is not supported by compiler", env['TARGET_OS'])

detectSystem.Finish()

if env.TargetOSIs('posix'):
    if env.ToolchainIs('gcc', 'clang'):
        env.Append(
            CCFLAGS_WERROR=["-Werror"],
            CXXFLAGS_WERROR=['-Werror=unused-result'] if env.ToolchainIs('clang') else [],
            LINKFLAGS_WERROR=[
                '-Wl,-fatal_warnings' if env.TargetOSIs('darwin') else "-Wl,--fatal-warnings"
            ],
        )
elif env.TargetOSIs('windows'):
    env.Append(CCFLAGS_WERROR=["/WX"])

if env.ToolchainIs('clang'):

    def assembler_with_cpp_gen(target, source, env, for_signature):
        if source[0].get_suffix() == '.sx':
            return '-x assembler-with-cpp'

    env['CLANG_ASSEMBLER_WITH_CPP'] = assembler_with_cpp_gen
    env.Append(ASFLAGS=['$CLANG_ASSEMBLER_WITH_CPP'])

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
# use the "static" mode. Someday later, we probably want to make the developer build default
# dynamic.
link_model = get_option('link-model')
if link_model == "auto":
    link_model = "static"

if link_model.startswith('dynamic') and get_option('install-action') == 'symlink':
    env.FatalError(
        f"Options '--link-model={link_model}' not supported with '--install-action={get_option('install-action')}'."
    )

if link_model == 'dynamic' and env.TargetOSIs(
        'darwin') and not get_option('force-macos-dynamic-link'):

    macos_version_message = textwrap.dedent("""\
        link-model=dynamic us only supported on macos version 12 or higher.
        This is due to a 512 dylib RUNTIME limit on older macos. See this post for
        more information: https://developer.apple.com/forums//thread/708366?login=true&page=1#717495022
        Use '--force-macos-dynamic-link' to bypass this check.
        """)

    try:
        macos_version_major = int(platform.mac_ver()[0].split('.')[0])
        if macos_version_major < 12:
            env.FatalError(
                textwrap.dedent(f"""\
                Macos version detected: {macos_version_major}
                """) + macos_version_message)
    except (IndexError, TypeError) as exc:
        env.FatalError(
            textwrap.dedent(f"""\
            Failed to detect macos version: {exc}
            """) + macos_version_message)

# libunwind configuration.
# In which the following globals are set and normalized to bool:
#     - use_libunwind
#     - use_system_libunwind
#     - use_vendored_libunwind
use_libunwind = get_option("use-libunwind")
use_system_libunwind = use_system_version_of_library("libunwind")

# Assume system libunwind works if it's installed and selected.
can_use_libunwind = (use_system_libunwind or env.TargetOSIs('linux') and
                     (env['TARGET_ARCH'] in ('x86_64', 'aarch64', 'ppc64le', 's390x')))

if use_libunwind == "off":
    use_libunwind = False
    use_system_libunwind = False
elif use_libunwind == "on":
    use_libunwind = True
    if not can_use_libunwind:
        env.ConfError("libunwind not supported on target platform")
        Exit(1)
elif use_libunwind == "auto":
    use_libunwind = can_use_libunwind

use_vendored_libunwind = use_libunwind and not use_system_libunwind
if use_system_libunwind and not use_libunwind:
    print("Error: --use-system-libunwind requires --use-libunwind")
    Exit(1)
if use_libunwind == True:
    env.SetConfigHeaderDefine("MONGO_CONFIG_USE_LIBUNWIND")

if get_option('visibility-support') == 'auto':
    visibility_annotations_enabled = (not env.TargetOSIs('windows')
                                      and link_model.startswith("dynamic"))
else:
    visibility_annotations_enabled = get_option('visibility-support') == 'on'

# Windows can't currently support anything other than 'object' or 'static', until
# we have annotated functions for export.
if env.TargetOSIs('windows') and not visibility_annotations_enabled:
    if link_model not in ['object', 'static', 'dynamic-sdk']:
        env.FatalError(
            "Windows builds must use the 'object', 'dynamic-sdk', or 'static' link models")

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

# Teach builders how to build idl files
for builder in ['SharedObject', 'StaticObject']:
    env['BUILDERS'][builder].add_src_builder("Idlc")

if link_model.startswith("dynamic"):

    if link_model == "dynamic" and visibility_annotations_enabled:

        def visibility_cppdefines_generator(target, source, env, for_signature):
            if not 'MONGO_API_NAME' in env:
                return None
            return "MONGO_API_${MONGO_API_NAME}"

        env['MONGO_VISIBILITY_CPPDEFINES_GENERATOR'] = visibility_cppdefines_generator

        def visibility_shccflags_generator(target, source, env, for_signature):
            if env.get('MONGO_API_NAME'):
                return "-fvisibility=hidden"
            return None

        if not env.TargetOSIs('windows'):
            env['MONGO_VISIBILITY_SHCCFLAGS_GENERATOR'] = visibility_shccflags_generator

        env.AppendUnique(
            CPPDEFINES=[
                'MONGO_USE_VISIBILITY',
                '$MONGO_VISIBILITY_CPPDEFINES_GENERATOR',
            ],
            SHCCFLAGS=[
                '$MONGO_VISIBILITY_SHCCFLAGS_GENERATOR',
            ],
        )

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
    # NOTE: The `illegal_cyclic_or_unresolved_dependencies_allowlisted`
    # tag can be applied to a library to indicate that it does not (or
    # cannot) completely express all of its required link dependencies.
    # This can occur for four reasons:
    #
    # - No unique provider for the symbol: Some symbols do not have a
    #   unique dependency that provides a definition, in which case it
    #   is impossible for the library to express a dependency edge to
    #   resolve the symbol.
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
            print("WARNING: Building MongoDB server with dynamic linking " +
                  "on macOS is not supported. Static linking is recommended.")

        if link_model == "dynamic-strict":
            # Darwin is strict by default
            pass
        else:

            def libdeps_tags_expand_incomplete(source, target, env, for_signature):
                # On darwin, since it is strict by default, we need to add a flag
                # when libraries are tagged incomplete.
                if ('illegal_cyclic_or_unresolved_dependencies_allowlisted' in
                        target[0].get_env().get("LIBDEPS_TAGS", [])):
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
                if ('illegal_cyclic_or_unresolved_dependencies_allowlisted' in
                        target[0].get_env().get("LIBDEPS_TAGS", [])):
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
                    if ('illegal_cyclic_or_unresolved_dependencies_allowlisted' not in
                            target[0].get_env().get("LIBDEPS_TAGS", [])):
                        return ["-Wl,-z,defs"]
                    return []

                env['LIBDEPS_TAG_EXPANSIONS'].append(libdeps_tags_expand_incomplete)

if optBuild != "off":
    env.SetConfigHeaderDefine("MONGO_CONFIG_OPTIMIZED_BUILD")

# Enable the fast decider if explicitly requested or if in 'auto' mode
# and not in conflict with other options like the ninja option which
# sets its own decider.
if (get_option('ninja') == 'disabled' and get_option('build-fast-and-loose') == 'on'
        or (get_option('build-fast-and-loose') == 'auto' and not has_option('release'))):
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
    env["CCCOM"] = env["CCCOM"].replace("$CCFLAGS", "$PROGCCFLAGS")
    env["CXXCOM"] = env["CXXCOM"].replace("$CCFLAGS", "$PROGCCFLAGS")
    env["PROGCCFLAGS"] = ['$CCFLAGS']

    env["CCCOM"] = env["CCCOM"].replace("$CFLAGS", "$PROGCFLAGS")
    env["PROGCFLAGS"] = ['$CFLAGS']

    env["CXXCOM"] = env["CXXCOM"].replace("$CXXFLAGS", "$PROGCXXFLAGS")
    env['PROGCXXFLAGS'] = ['$CXXFLAGS']

    env["LINKCOM"] = env["LINKCOM"].replace("$LINKFLAGS", "$PROGLINKFLAGS")
    env["PROGLINKFLAGS"] = ['$LINKFLAGS']

# When it is necessary to supply additional SHLINKFLAGS without modifying the toolset default,
# following appends contents of SHLINKFLAGS_EXTRA variable to the linker command
env.AppendUnique(SHLINKFLAGS=['$SHLINKFLAGS_EXTRA'])


class ForceVerboseConftest():
    """
    This class allows for configurable substition calls to enable forcing
    the conftest to use verbose logs even when verbose mode is not specified.
    """

    def __init__(self, msg):
        self.msg = msg

    def __call__(self, target, source, env, for_signature):
        for t in target:
            # TODO: SERVER-60915 switch to SCons api conftest check
            if 'conftest' in str(t):
                return None
        return self.msg


if not env.Verbose():
    # Even though we are not in Verbose mode, conftest logs should
    # always be verbose, because they go to a file and not seen
    # by the user anyways.
    env.Append(CCCOMSTR=ForceVerboseConftest("Compiling $TARGET"))
    env.Append(CXXCOMSTR=ForceVerboseConftest(env["CCCOMSTR"]))
    env.Append(SHCCCOMSTR=ForceVerboseConftest("Compiling $TARGET"))
    env.Append(SHCXXCOMSTR=ForceVerboseConftest(env["SHCCCOMSTR"]))
    env.Append(LINKCOMSTR=ForceVerboseConftest("Linking $TARGET"))
    env.Append(SHLINKCOMSTR=ForceVerboseConftest(env["LINKCOMSTR"]))
    env.Append(ARCOMSTR=ForceVerboseConftest("Generating library $TARGET"))

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

    # We originally did this by setting ARCOM to write_uuid_to_file.
    # This worked more or less by accident. It works when SCons is
    # doing the action execution because when it would subst the
    # command line subst would execute the function as part of string
    # resolution which would have the side effect of writing the
    # file. Since it returned None subst would do some special
    # handling to make sure it never made it to the command line. This
    # breaks Ninja however because we are taking that return value and
    # trying to pass it to the command executor (/bin/sh or
    # cmd.exe) and end up with the function name as a command. The
    # resulting command looks something like `/bin/sh -c
    # 'write_uuid_to_file(env, target, source)`. If we instead
    # actually do what we want and that is make the StaticLibrary
    # builder's action a FunctionAction the Ninja generator will
    # correctly dispatch it and not generate an invalid command
    # line. This also has the side benefit of being more clear that
    # we're expecting a Python function to execute here instead of
    # pretending to be a CommandAction that just happens to not run a
    # command but instead runs a function.
    env["BUILDERS"]["StaticLibrary"].action = SCons.Action.Action(
        write_uuid_to_file, "Generating placeholder library $TARGET")

import libdeps_tool as libdeps

libdeps.setup_environment(
    env,
    emitting_shared=(link_model.startswith("dynamic")),
    debug=get_option('libdeps-debug'),
    linting=get_option('libdeps-linting'),
)

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

    if env.TargetOSIs('darwin') and env.get('TAPI'):
        tapilink = Tool('tapilink')
        if tapilink.exists(env):
            tapilink(env)

if env['_LIBDEPS'] == '$_LIBDEPS_LIBS':
    # The following platforms probably aren't using the binutils
    # toolchain, or may be using it for the archiver but not the
    # linker, and binutils currently is the only thing that supports
    # thin archives. Don't even try on those platforms.
    if not env.TargetOSIs('solaris', 'darwin', 'windows', 'openbsd'):
        env.Tool('thin_archive')

if env.TargetOSIs('linux', 'freebsd', 'openbsd'):
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,--whole-archive'
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = '-Wl,--no-whole-archive'
    env['LINK_AS_NEEDED_LIB_START'] = '-Wl,--as-needed'
    env['LINK_AS_NEEDED_LIB_END'] = '-Wl,--no-as-needed'
elif env.TargetOSIs('darwin'):
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,-force_load'
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ''
    env['LINK_AS_NEEDED_LIB_START'] = '-Wl,-mark_dead_strippable_dylib'
    env['LINK_AS_NEEDED_LIB_END'] = ''
elif env.TargetOSIs('solaris'):
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '-Wl,-z,allextract'
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = '-Wl,-z,defaultextract'
elif env.TargetOSIs('windows'):
    env['LINK_WHOLE_ARCHIVE_LIB_START'] = '/WHOLEARCHIVE'
    env['LINK_WHOLE_ARCHIVE_LIB_END'] = ''
    env['LIBDEPS_FLAG_SEPARATORS'] = {env['LINK_WHOLE_ARCHIVE_LIB_START']: {'suffix': ':'}}

if env.TargetOSIs('darwin') and link_model.startswith('dynamic'):

    def init_no_global_libdeps_tag_expansion(source, target, env, for_signature):
        """
        This callable will be expanded by scons and modify the environment by
        adjusting the prefix and postfix flags to account for linking options
        related to the use of global static initializers for any given libdep.
        """

        if "init-no-global-side-effects" in env.get(libdeps.Constants.LibdepsTags, []):
            # macos as-needed flag is used on the library directly when it is built
            return env.get('LINK_AS_NEEDED_LIB_START', '')

    env['LIBDEPS_TAG_EXPANSIONS'].append(init_no_global_libdeps_tag_expansion)


def init_no_global_add_flags(target, start_flag, end_flag):
    """ Helper function for init_no_global_libdeps_tag_expand"""

    setattr(target[0].attributes, "libdeps_prefix_flags", [start_flag])
    setattr(target[0].attributes, "libdeps_postfix_flags", [end_flag])
    if env.TargetOSIs('linux', 'freebsd', 'openbsd'):
        setattr(
            target[0].attributes,
            "libdeps_switch_flags",
            [{
                'on': start_flag,
                'off': end_flag,
            }],
        )


def init_no_global_libdeps_tag_emitter(target, source, env):
    """
    This emitter will be attached the correct pre and post fix flags to
    a given library to cause it to have certain flags before or after on the link
    line.
    """

    if link_model == 'dynamic':
        start_flag = env.get('LINK_AS_NEEDED_LIB_START', '')
        end_flag = env.get('LINK_AS_NEEDED_LIB_END', '')

        # In the dynamic case, any library that is known to not have global static
        # initializers can supply the flag and be wrapped in --as-needed linking,
        # allowing the linker to be smart about linking libraries it may not need.
        if ("init-no-global-side-effects" in env.get(libdeps.Constants.LibdepsTags, [])
                and not env.TargetOSIs('darwin')):
            init_no_global_add_flags(target, start_flag, end_flag)
        else:
            init_no_global_add_flags(target, "", "")

    else:
        start_flag = env.get('LINK_WHOLE_ARCHIVE_LIB_START', '')
        end_flag = env.get('LINK_WHOLE_ARCHIVE_LIB_END', '')

        # In the static case, any library that is unknown to have global static
        # initializers should supply the flag and be wrapped in --whole-archive linking,
        # allowing the linker to bring in all those symbols which may not be directly needed
        # at link time.
        if "init-no-global-side-effects" not in env.get(libdeps.Constants.LibdepsTags, []):
            init_no_global_add_flags(target, start_flag, end_flag)
        else:
            init_no_global_add_flags(target, "", "")
    return target, source


for target_builder in ['SharedLibrary', 'SharedArchive', 'StaticLibrary']:
    builder = env['BUILDERS'][target_builder]
    base_emitter = builder.emitter
    new_emitter = SCons.Builder.ListEmitter([base_emitter, init_no_global_libdeps_tag_emitter])
    builder.emitter = new_emitter

link_guard_rules = {
    "test": ["dist", ],
}


class LibdepsLinkGuard(SCons.Errors.UserError):
    pass


def checkComponentType(target_comps, comp, target, lib):
    """
    For a libdep and each AIB_COMPONENT its labeled as, check if its violates
    any of the link gaurd rules.
    """
    for target_comp in target_comps:
        for link_guard_rule in link_guard_rules:
            if (target_comp in link_guard_rules[link_guard_rule] and link_guard_rule in comp):
                raise LibdepsLinkGuard(
                    textwrap.dedent(f"""\n
                    LibdepsLinkGuard:
                    \tTarget '{target[0]}' links LIBDEP '{lib}'
                    \tbut is listed as AIB_COMPONENT '{target_comp}' which is not allowed link libraries
                    \twith AIB_COMPONENTS that include the word '{link_guard_rule}'\n"""))


def get_comps(env):
    """util function for extracting all AIB_COMPONENTS as a list"""
    comps = env.get("AIB_COMPONENTS_EXTRA", [])
    comp = env.get("AIB_COMPONENT", None)
    if comp:
        comps += [comp]
    return comps


def link_guard_libdeps_tag_expand(source, target, env, for_signature):
    """
    Callback function called on all binaries to check if a certain binary
    from a given component is linked to another binary of a given component,
    the goal being to define rules that prevents test components from being
    linked into production or releaseable components.
    """
    for lib in libdeps.get_libdeps(source, target, env, for_signature):
        if not lib.env:
            continue

        for comp in get_comps(lib.env):
            checkComponentType(get_comps(env), comp, target, lib)

    return []


env['LIBDEPS_TAG_EXPANSIONS'].append(link_guard_libdeps_tag_expand)

env.Tool('forceincludes')

# ---- other build setup -----
if debugBuild:
    env.SetConfigHeaderDefine("MONGO_CONFIG_DEBUG_BUILD")
else:
    env.AppendUnique(CPPDEFINES=['NDEBUG'])

# Normalize our experimental optimiation and hardening flags
selected_experimental_optimizations = set()
for suboption in get_option('experimental-optimization'):
    if suboption == "*":
        selected_experimental_optimizations.update(experimental_optimizations)
    elif suboption.startswith('-'):
        selected_experimental_optimizations.discard(suboption[1:])
    elif suboption.startswith('+'):
        selected_experimental_optimizations.add(suboption[1:])

selected_experimental_runtime_hardenings = set()
for suboption in get_option('experimental-runtime-hardening'):
    if suboption == "*":
        selected_experimental_runtime_hardenings.update(experimental_runtime_hardenings)
    elif suboption.startswith('-'):
        selected_experimental_runtime_hardenings.discard(suboption[1:])
    elif suboption.startswith('+'):
        selected_experimental_runtime_hardenings.add(suboption[1:])

if env.TargetOSIs('linux'):
    env.Append(LIBS=["m"])
    if not env.TargetOSIs('android'):
        env.Append(LIBS=["resolv"])

elif env.TargetOSIs('solaris'):
    env.Append(LIBS=["socket", "resolv", "lgrp"])

elif env.TargetOSIs('freebsd'):
    env.Append(LIBS=["kvm"])
    env.Append(CCFLAGS=["-fno-omit-frame-pointer"])

elif env.TargetOSIs('darwin'):
    env.Append(LIBS=["resolv"])

elif env.TargetOSIs('openbsd'):
    env.Append(LIBS=["kvm"])

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

    env.Append(CPPDEFINES=["_UNICODE"])
    env.Append(CPPDEFINES=["UNICODE"])

    # Temporary fixes to allow compilation with VS2017
    env.Append(CPPDEFINES=[
        "_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING",
        "_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",

        # TODO(SERVER-60151): Until we are fully in C++20 mode, it is
        # easier to simply suppress C++20 deprecations. After we have
        # switched over we should address any actual deprecated usages
        # and then remove this flag.
        "_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS",
    ])

    # /EHsc exception handling style for visual studio
    # /W3 warning level
    env.Append(CCFLAGS=["/EHsc", "/W3"])

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
        # base class.
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

        # C4251: This warning attempts to prevent usage of CRT (C++
        # standard library) types in DLL interfaces. That is a good
        # idea for DLLs you ship to others, but in our case, we know
        # that all DLLs are built consistently. Suppress the warning.
        "/wd4251",
    ])

    # mozjs requires the following
    #  'declaration' : no matching operator delete found; memory will not be freed if
    #  initialization throws an exception
    env.Append(CCFLAGS=["/wd4291"])

    # some warnings we should treat as errors:
    # c4013
    #  'function' undefined; assuming extern returning int
    #    This warning occurs when files compiled for the C language use functions not defined
    #    in a header file.
    # c4099
    #  'identifier' : type name first seen using 'objecttype1' now seen using 'objecttype2'
    #    This warning occurs when classes and structs are declared with a mix of struct and class
    #    which can cause linker failures
    # c4930
    #  'identifier': prototyped function not called (was a variable definition intended?)
    #     This warning indicates a most-vexing parse error, where a user declared a function that
    #     was probably intended as a variable definition.  A common example is accidentally
    #     declaring a function called lock that takes a mutex when one meant to create a guard
    #     object called lock on the stack.
    env.Append(CCFLAGS=["/we4013", "/we4099", "/we4930"])

    env.Append(CPPDEFINES=[
        "_CONSOLE",
        "_CRT_SECURE_NO_WARNINGS",
        "_ENABLE_EXTENDED_ALIGNED_STORAGE",
        "_SCL_SECURE_NO_WARNINGS",
    ])

    # this would be for pre-compiled headers, could play with it later
    #env.Append( CCFLAGS=['/Yu"pch.h"'] )

    # Don't send error reports in case of internal compiler error
    env.Append(CCFLAGS=["/errorReport:none"])

    # Select debugging format. /Zi gives faster links but seems to use more memory.
    if get_option('msvc-debugging-format') == "codeview":
        env['CCPDBFLAGS'] = "/Z7"
    elif get_option('msvc-debugging-format') == "pdb":
        env['CCPDBFLAGS'] = '/Zi /Fd${TARGET}.pdb'

    # The SCons built-in pdbGenerator always adds /DEBUG, but we would like
    # control over that flag so that users can override with /DEBUG:fastlink
    # for better local builds. So we overwrite the builtin.
    def pdbGenerator(env, target, source, for_signature):
        try:
            return ['/PDB:%s' % target[0].attributes.pdb]
        except (AttributeError, IndexError):
            return None

    env['_PDB'] = pdbGenerator

    # /DEBUG will tell the linker to create a .pdb file
    # which WinDbg and Visual Studio will use to resolve
    # symbols if you want to debug a release-mode image.
    # Note that this means we can't do parallel links in the build.
    #
    # Please also note that this has nothing to do with _DEBUG or optimization.

    # If the user set a /DEBUG flag explicitly, don't add
    # another. Otherwise use the standard /DEBUG flag, since we always
    # want PDBs.
    if not any(flag.startswith('/DEBUG') for flag in env['LINKFLAGS']):
        env.Append(LINKFLAGS=["/DEBUG"])

    # /MD:  use the multithreaded, DLL version of the run-time library (MSVCRT.lib/MSVCR###.DLL)
    # /MDd: Defines _DEBUG, _MT, _DLL, and uses MSVCRTD.lib/MSVCRD###.DLL
    env.Append(CCFLAGS=["/MDd" if debugBuild else "/MD"])

    if optBuild == "off":
        env.Append(CCFLAGS=["/Od"])
        if debugBuild:
            # /RTC1: - Enable Stack Frame Run-Time Error Checking; Reports when a variable is used
            # without having been initialized (implies /Od: no optimizations)
            env.Append(CCFLAGS=["/RTC1"])
    else:
        # /O1:  optimize for size
        # /O2:  optimize for speed (as opposed to size)
        # /Oy-: disable frame pointer optimization (overrides /O2, only affects 32-bit)
        # /INCREMENTAL: NO - disable incremental link - avoid the level of indirection for function
        # calls

        optFlags = []
        if optBuild == "size":
            optFlags += ["/Os"]
        elif optBuild == "debug":
            optFlags += ["/Ox", "/Zo"]
        else:
            optFlags += ["/O2"]
        optFlags += ["/Oy-"]

        env.Append(CCFLAGS=optFlags)
        env.Append(LINKFLAGS=["/INCREMENTAL:NO"])

    # Support large object files since some unit-test sources contain a lot of code
    env.Append(CCFLAGS=["/bigobj"])

    # Set Source and Executable character sets to UTF-8, this will produce a warning C4828 if the
    # file contains invalid UTF-8.
    env.Append(CCFLAGS=["/utf-8"])

    # Specify standards conformance mode to the compiler.
    env.Append(CCFLAGS=["/permissive-"])

    # Enables the __cplusplus preprocessor macro to report an updated value for recent C++ language
    # standards support.
    env.Append(CCFLAGS=["/Zc:__cplusplus"])

    # Tells the compiler to preferentially call global operator delete or operator delete[]
    # functions that have a second parameter of type size_t when the size of the object is available.
    env.Append(CCFLAGS=["/Zc:sizedDealloc"])

    # Treat volatile according to the ISO standard and do not guarantee acquire/release semantics.
    env.Append(CCFLAGS=["/volatile:iso"])

    # Tell CL to produce more useful error messages.
    env.Append(CCFLAGS=["/diagnostics:caret"])

    # This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds.
    env.Append(LINKFLAGS=["/LARGEADDRESSAWARE"])

    env.Append(
        LIBS=[
            'DbgHelp',
            'Iphlpapi',
            'Psapi',
            'advapi32',
            'bcrypt',
            'crypt32',
            'dnsapi',
            'kernel32',
            'shell32',
            'pdh',
            'version',
            'winmm',
            'ws2_32',
            'secur32',
        ], )

# When building on visual studio, this sets the name of the debug symbols file
if env.ToolchainIs('msvc'):
    env['PDB'] = '${TARGET.base}.pdb'

# Python uses APPDATA to determine the location of user installed
# site-packages. If we do not pass this variable down to Python
# subprocesses then anything installed with `pip install --user`
# will be inaccessible leading to import errors.
#
# Use env['PLATFORM'] instead of TargetOSIs since we always want this
# to run on Windows hosts but not always for Windows targets.
if env['PLATFORM'] == 'win32':
    appdata = os.getenv('APPDATA', None)
    if appdata is not None:
        env['ENV']['APPDATA'] = appdata

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
    # Furthermore, as both C++ compilers appear to define _GNU_SOURCE
    # unconditionally (because libstdc++ requires it), it seems
    # prudent to explicitly add that too, so that C language checks
    # see a consistent set of definitions.
    if env.TargetOSIs('linux'):
        env.AppendUnique(CPPDEFINES=[
            ('_XOPEN_SOURCE', 700),
            '_GNU_SOURCE',
        ], )

    # If shared and static object files stripped of their rightmost
    # dot-delimited suffix would collide, modify the shared library
    # ones so that they won't. We do this because if split dwarf is in
    # play, static and dynamic builds would otherwise overwrite each
    # other's .dwo files, because GCC strips the last suffix and adds
    # .dwo, rather than simply appending .dwo to the full filename.
    objsuffelts = env.subst('$OBJSUFFIX').split('.')
    shobjsuffelts = env.subst('$SHOBJSUFFIX').split('.')
    if objsuffelts[0:-1] == shobjsuffelts[0:-1]:
        env['SHOBJSUFFIX'] = '.dyn${OBJSUFFIX}'

    # Everything on OS X is position independent by default.
    if not env.TargetOSIs('darwin'):
        if get_option('runtime-hardening') == "on":
            # If runtime hardening is requested, then build anything
            # destined for an executable with the necessary flags for PIE.
            env.AppendUnique(
                PROGCCFLAGS=['-fPIE'],
                PROGLINKFLAGS=['-pie'],
            )

    # -Winvalid-pch Warn if a precompiled header (see Precompiled Headers) is found in the search path but can't be used.
    env.Append(
        CCFLAGS=[
            "-fasynchronous-unwind-tables",
            "-ggdb" if not env.TargetOSIs('emscripten') else "-g",
            "-Wall",
            "-Wsign-compare",
            "-Wno-unknown-pragmas",
            "-Winvalid-pch",
        ], )

    # TODO: At least on x86, glibc as of 2.3.4 will consult the
    # .eh_frame info via _Unwind_Backtrace to do backtracing without
    # needing the frame pointer, despite what the backtrace man page
    # actually says. We should see if we can drop the requirement that
    # we use libunwind here.
    can_nofp = (env.TargetOSIs('darwin') or use_libunwind)

    # For debug builds with tcmalloc, we need the frame pointer so it can
    # record the stack of allocations.
    can_nofp &= not (debugBuild and (env['MONGO_ALLOCATOR'] == 'tcmalloc'))

    # Only disable frame pointers if requested
    can_nofp &= ("nofp" in selected_experimental_optimizations)

    if not can_nofp:
        env.Append(CCFLAGS=["-fno-omit-frame-pointer"])

    if not "tbaa" in selected_experimental_optimizations:
        env.Append(CCFLAGS=["-fno-strict-aliasing"])

    # Enabling hidden visibility on non-darwin requires that we have
    # libunwind in play, since glibc backtrace will not work
    # correctly.
    if "vishidden" in selected_experimental_optimizations and (env.TargetOSIs('darwin')
                                                               or use_libunwind):
        if link_model.startswith('dynamic'):
            # In dynamic mode, we can't make the default visibility
            # hidden because not all libraries have export tags. But
            # we can at least make inlines hidden.
            #
            # TODO: Except on macOS, where we observe lots of crashes
            # when we enable this. We should investigate further but
            # it isn't relevant for the purpose of exploring these
            # flags on linux, where they seem to work fine.
            if not env.TargetOSIs('darwin'):
                env.Append(CXXFLAGS=["-fvisibility-inlines-hidden"])
        else:
            # In static mode, we need an escape hatch for a few
            # libraries that don't work correctly when built with
            # hidden visiblity.
            def conditional_visibility_generator(target, source, env, for_signature):
                if 'DISALLOW_VISHIDDEN' in env:
                    return
                return "-fvisibility=hidden"

            env.Append(
                CCFLAGS_VISIBILITY_HIDDEN_GENERATOR=conditional_visibility_generator,
                CCFLAGS='$CCFLAGS_VISIBILITY_HIDDEN_GENERATOR',
            )

    # env.Append( " -Wconversion" ) TODO: this doesn't really work yet
    env.Append(CXXFLAGS=["-Woverloaded-virtual"])

    # On OS X, clang doesn't want the pthread flag at link time, or it
    # issues warnings which make it impossible for us to declare link
    # warnings as errors. See http://stackoverflow.com/a/19382663.
    if not (env.TargetOSIs('darwin') and env.ToolchainIs('clang')):
        env.Append(LINKFLAGS=["-pthread"])

    # SERVER-9761: Ensure early detection of missing symbols in dependent libraries at program
    # startup.
    env.Append(LINKFLAGS=[
        "-Wl,-bind_at_load" if env.TargetOSIs('macOS') else "-Wl,-z,now",
    ], )

    # We need to use rdynamic for backtraces with glibc unless we have libunwind.
    nordyn = (env.TargetOSIs('darwin') or use_libunwind)

    # And of course only do rdyn if the experimenter asked for it.
    nordyn &= ("nordyn" in selected_experimental_optimizations)

    if nordyn:

        def export_symbol_generator(source, target, env, for_signature):
            symbols = copy.copy(env.get('EXPORT_SYMBOLS', []))
            for lib in libdeps.get_libdeps(source, target, env, for_signature):
                if lib.env:
                    symbols.extend(lib.env.get('EXPORT_SYMBOLS', []))
            export_expansion = '${EXPORT_SYMBOL_FLAG}'
            return [f'-Wl,{export_expansion}{symbol}' for symbol in symbols]

        env['EXPORT_SYMBOL_GEN'] = export_symbol_generator

        # For darwin, we need the leading underscore but for others we
        # don't. Hacky but it works to jam that distinction into the
        # flag itself, since it already differs on darwin.
        if env.TargetOSIs('darwin'):
            env['EXPORT_SYMBOL_FLAG'] = "-exported_symbol,_"
        else:
            env['EXPORT_SYMBOL_FLAG'] = "--export-dynamic-symbol,"

        env.Append(PROGLINKFLAGS=[
            '$EXPORT_SYMBOL_GEN',
        ], )
    elif not env.TargetOSIs('darwin'):
        env.Append(PROGLINKFLAGS=[
            "-rdynamic",
        ], )

    #make scons colorgcc friendly
    for key in ('HOME', 'TERM'):
        try:
            env['ENV'][key] = os.environ[key]
        except KeyError:
            pass

    if has_option("gcov"):
        if not (env.TargetOSIs('linux') and (env.ToolchainIs('gcc', 'clang'))):
            # TODO: This should become supported under: https://jira.mongodb.org/browse/SERVER-49877
            env.FatalError(
                "Coverage option 'gcov' is currently only supported on linux with gcc and clang. See SERVER-49877."
            )

        env.AppendUnique(
            CCFLAGS=['--coverage'],
            LINKFLAGS=['--coverage'],
        )

    if optBuild == "off":
        env.Append(CCFLAGS=["-O0"])
    else:
        if optBuild == "size":
            env.Append(CCFLAGS=["-Os"])
        elif optBuild == "debug":
            env.Append(CCFLAGS=["-Og"])
        else:
            if "O3" in selected_experimental_optimizations:
                env.Append(CCFLAGS=["-O3"])
            else:
                env.Append(CCFLAGS=["-O2"])

        if "treevec" in selected_experimental_optimizations:
            env.Append(CCFLAGS=["-ftree-vectorize"])

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

if get_option('ocsp-stapling') == 'on':
    # OCSP Stapling needs to be disabled on ubuntu 18.04 machines because when TLS 1.3 is
    # enabled on that machine, the status-response message sent contains garbage data. This
    # is a known bug and needs to be fixed by upstream, but for the time being we need to
    # disable OCSP Stapling on Ubuntu 18.04 machines. See SERVER-51364 for more details.
    env.SetConfigHeaderDefine("MONGO_CONFIG_OCSP_STAPLING_ENABLED")

if not env.TargetOSIs('windows', 'macOS') and (env.ToolchainIs('GCC', 'clang')):

    # By default, apply our current microarchitecture minima. If the
    # user has customized a flag of the same name in any of CCFLAGS,
    # CFLAGS, or CXXFLAGS, we disable applying our default to
    # CCFLAGS. We are assuming the user knows what they are doing,
    # e.g. we don't try to be smart and notice that they applied it to
    # CXXFLAGS and therefore still apply it to CFLAGS since they
    # didn't customize that. Basically, don't send those flags in
    # unless you a) mean it, and b) know what you are doing, and c)
    # cover all your bases by either setting it via CCFLAGS, or
    # setting it for both C and C++ by setting both of CFLAGS and
    # CXXFLAGS.

    default_targeting_flags_for_architecture = {
        "aarch64": {"-march=": "armv8.2-a", "-mtune=": "generic"},
        "i386": {"-march=": "nocona", "-mtune=": "generic"},
        "ppc64le": {"-mcpu=": "power8", "-mtune=": "power8", "-mcmodel=": "medium"},
        "s390x": {"-march=": "z196", "-mtune=": "zEC12"},
    }

    # If we are enabling vectorization in sandybridge mode, we'd
    # rather not hit the 256 wide vector instructions because the
    # heavy versions can cause clock speed reductions.
    if "sandybridge" in selected_experimental_optimizations:
        default_targeting_flags_for_architecture["x86_64"] = {
            "-march=": "sandybridge",
            "-mtune=": "generic",
            "-mprefer-vector-width=": "128",
        }

    default_targeting_flags = default_targeting_flags_for_architecture.get(env['TARGET_ARCH'])
    if default_targeting_flags:
        search_variables = ['CCFLAGS', 'CFLAGS', 'CXXFLAGS']
        for targeting_flag, targeting_flag_value in default_targeting_flags.items():
            if not any(
                    flag_value.startswith(targeting_flag) for search_variable in search_variables
                    for flag_value in env[search_variable]):
                env.Append(CCFLAGS=[f'{targeting_flag}{targeting_flag_value}'])

# Needed for auth tests since key files are stored in git with mode 644.
if not env.TargetOSIs('windows'):
    for keysuffix in ["1", "2", "ForRollover"]:
        keyfile = "jstests/libs/key%s" % keysuffix
        os.chmod(keyfile, stat.S_IWUSR | stat.S_IRUSR)

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

has_ninja_module = False
for module in mongo_modules:
    if hasattr(module, 'NinjaFile'):
        has_ninja_module = True
        break

if get_option('ninja') != 'disabled' and has_ninja_module:
    env.FatalError(
        textwrap.dedent("""\
        ERROR: Ninja tool option '--ninja' should not be used with the ninja module.
            Using both options simultaneously may clobber build.ninja files.
            Remove the ninja module directory or use '--modules= ' to select no modules.
            If using enterprise module, explicitly set '--modules=<name-of-enterprise-module>' to exclude the ninja module."""
                        ))

if has_ninja_module:
    print(
        "WARNING: You are attempting to use the unsupported/legacy ninja module, instead of the integrated ninja generator. You are strongly encouraged to remove the ninja module from your module list and invoke scons with --ninja generate-ninja"
    )

# --- check system ---
ssl_provider = None
free_monitoring = get_option("enable-free-mon")
http_client = get_option("enable-http-client")


def isSanitizerEnabled(self, sanitizerName):
    if 'SANITIZERS_ENABLED' not in self:
        return False
    if sanitizerName == 'fuzzer':
        return 'fuzzer-no-link' in self['SANITIZERS_ENABLED']
    return sanitizerName in self['SANITIZERS_ENABLED']


env.AddMethod(isSanitizerEnabled, 'IsSanitizerEnabled')


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
        compiler_minimum_string = "Microsoft Visual Studio 2022 17.0"
        compiler_test_body = textwrap.dedent("""
        #if !defined(_MSC_VER)
        #error
        #endif

        #if _MSC_VER < 1930
        #error %s or newer is required to build MongoDB
        #endif

        int main(int argc, char* argv[]) {
            return 0;
        }
        """ % compiler_minimum_string)
    elif myenv.ToolchainIs('gcc'):
        if get_option('cxx-std') == "20":
            compiler_minimum_string = "GCC 11.2"
            compiler_test_body = textwrap.dedent("""
            #if !defined(__GNUC__) || defined(__clang__)
            #error
            #endif

            #if (__GNUC__ < 11) || (__GNUC__ == 11 && __GNUC_MINOR__ < 2)
            #error %s or newer is required to build MongoDB
            #endif

            int main(int argc, char* argv[]) {
                return 0;
            }
            """ % compiler_minimum_string)
        else:
            compiler_minimum_string = "GCC 8.2"
            compiler_test_body = textwrap.dedent("""
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
        if get_option('cxx-std') == "20":
            compiler_minimum_string = "clang 12.0 (or Apple XCode 13.0)"
            compiler_test_body = textwrap.dedent("""
            #if !defined(__clang__)
            #error
            #endif

            #if defined(__apple_build_version__)
            #if __apple_build_version__ < 13000029
            #error %s or newer is required to build MongoDB
            #endif
            #elif (__clang_major__ < 12) || (__clang_major__ == 12 && __clang_minor__ < 0)
            #error %s or newer is required to build MongoDB
            #endif

            int main(int argc, char* argv[]) {
                return 0;
            }
            """ % (compiler_minimum_string, compiler_minimum_string))
        else:
            compiler_minimum_string = "clang 7.0 (or Apple XCode 13.0)"
            compiler_test_body = textwrap.dedent("""
            #if !defined(__clang__)
            #error
            #endif

            #if defined(__apple_build_version__)
            #if __apple_build_version__ < 13000029
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
            "C": ".c",
            "C++": ".cpp",
        }
        context.Message(
            "Checking if %s compiler is %s or newer..." % (language, compiler_minimum_string))
        result = context.TryCompile(compiler_test_body, extension_for[language])
        context.Result(result)
        return result

    conf = Configure(
        myenv,
        help=False,
        custom_tests={
            'CheckForMinimumCompiler': CheckForMinimumCompiler,
        },
    )

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
            # If no minimum version has been specified, use our default.
            win_version_min = 'win10'

        env['WIN_VERSION_MIN'] = win_version_min
        win_version_min = win_version_min_choices[win_version_min]
        env.Append(CPPDEFINES=[("_WIN32_WINNT", "0x" + win_version_min[0])])
        env.Append(CPPDEFINES=[("BOOST_USE_WINAPI_VERSION", "0x" + win_version_min[0])])
        env.Append(CPPDEFINES=[("NTDDI_VERSION", "0x" + win_version_min[0] + win_version_min[1])])

    conf.Finish()

    # We require macOS 10.14 or newer
    if env.TargetOSIs('darwin'):

        # TODO: Better error messages, mention the various -mX-version-min-flags in the error, and
        # single source of truth for versions, plumbed through #ifdef ladder and error messages.
        def CheckDarwinMinima(context):
            test_body = """
            #include <Availability.h>
            #include <AvailabilityMacros.h>
            #include <TargetConditionals.h>

            #if TARGET_OS_OSX && (__MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_14)
            #error 1
            #endif
            """

            context.Message("Checking for sufficient {0} target version minimum... ".format(
                context.env['TARGET_OS']))
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                "CheckDarwinMinima": CheckDarwinMinima,
            },
        )

        if not conf.CheckDarwinMinima():
            conf.env.ConfError("Required target minimum of macOS 10.14 not found")

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

        # If the user has selected ``configure` in
        # `disable-warnings-as-errors`, the usual mechanisms that
        # would inject Werror or similar are disabled for
        # conftests. But AddFlagIfSupported requires that those flags
        # be used. Disable the generators so we have explicit control.
        cloned = env.Clone(
            CCFLAGS_GENERATE_WERROR=[],
            CXXFLAGS_GENERATE_WERROR=[],
            LINKFLAGS_GENERATE_WERROR=[],
        )

        cloned.Append(**test_mutation)

        # Add these *after* the test mutation, so that the mutation
        # can't step on the warnings-as-errors state.
        cloned.Append(
            CCFLAGS=["$CCFLAGS_WERROR"],
            CXXFLAGS=["$CXXFLAGS_WERROR"],
            LINKFLAGS=["$LINKFLAGS_WERROR"],
        )

        conf = Configure(
            cloned,
            help=False,
            custom_tests={
                'CheckFlag': lambda ctx: CheckFlagTest(ctx, tool, extension, flag),
            },
        )
        available = conf.CheckFlag()
        conf.Finish()
        if available:
            env.Append(**mutation)
        return available

    def AddToCFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, False, CFLAGS=[flag])

    env.AddMethod(AddToCFLAGSIfSupported)

    def AddToCCFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, False, CCFLAGS=[flag])

    env.AddMethod(AddToCCFLAGSIfSupported)

    def AddToCXXFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C++', '.cpp', flag, False, CXXFLAGS=[flag])

    env.AddMethod(AddToCXXFLAGSIfSupported)

    def AddToLINKFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, True, LINKFLAGS=[flag])

    env.AddMethod(AddToLINKFLAGSIfSupported)

    def AddToSHLINKFLAGSIfSupported(env, flag):
        return AddFlagIfSupported(env, 'C', '.c', flag, True, SHLINKFLAGS=[flag])

    env.AddMethod(AddToSHLINKFLAGSIfSupported)

    if myenv.ToolchainIs('gcc', 'clang'):
        # This tells clang/gcc to use the gold linker if it is available - we prefer the gold linker
        # because it is much faster. Don't use it if the user has already configured another linker
        # selection manually.
        if any(flag.startswith('-fuse-ld=') for flag in env['LINKFLAGS']):
            myenv.FatalError(
                f"Use the '--linker' option instead of modifying the LINKFLAGS directly.")

        linker_ld = get_option('linker')
        if linker_ld == 'auto':
            # lld has problems with separate debug info on some platforms. See:
            # - https://bugzilla.mozilla.org/show_bug.cgi?id=1485556
            # - https://bugzilla.mozilla.org/show_bug.cgi?id=1485556
            #
            # lld also apparently has problems with symbol resolution
            # in some esoteric configurations that apply for us when
            # using --link-model=dynamic mode, so disable lld there
            # too. See:
            # - https://bugs.llvm.org/show_bug.cgi?id=46676
            #
            # We should revisit all of these issues the next time we upgrade our clang minimum.
            if get_option('separate-debug') == 'off' and get_option('link-model') != 'dynamic':
                if not AddToLINKFLAGSIfSupported(myenv, '-fuse-ld=lld'):
                    AddToLINKFLAGSIfSupported(myenv, '-fuse-ld=gold')
            else:
                AddToLINKFLAGSIfSupported(myenv, '-fuse-ld=gold')
        elif link_model.startswith("dynamic") and linker_ld == 'bfd':
            # BFD is not supported due to issues with it causing warnings from some of
            # the third party libraries that mongodb is linked with:
            # https://jira.mongodb.org/browse/SERVER-49465
            myenv.FatalError(f"Linker {linker_ld} is not supported with dynamic link model builds.")
        else:
            if not AddToLINKFLAGSIfSupported(myenv, f'-fuse-ld={linker_ld}'):
                myenv.FatalError(f"Linker {linker_ld} could not be configured.")

        if has_option('gcov') and AddToCCFLAGSIfSupported(myenv, '-fprofile-update=single'):
            myenv.AppendUnique(LINKFLAGS=['-fprofile-update=single'])

    detectCompiler = Configure(
        myenv,
        help=False,
        custom_tests={
            'CheckForCXXLink': CheckForCXXLink,
        },
    )

    if not detectCompiler.CheckCC():
        env.ConfError(
            "C compiler {0} doesn't work",
            detectEnv['CC'],
        )

    if not detectCompiler.CheckCXX():
        env.ConfError(
            "C++ compiler {0} doesn't work",
            detectEnv['CXX'],
        )

    if not detectCompiler.CheckForCXXLink():
        env.ConfError(
            "C++ compiler {0} can't link C++ programs",
            detectEnv['CXX'],
        )

    detectCompiler.Finish()

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

        # Enable sized deallocation support.
        AddToCXXFLAGSIfSupported(myenv, '-fsized-deallocation')

        # This warning was added in Apple clang version 11 and flags many explicitly defaulted move
        # constructors and assignment operators for being implicitly deleted, which is not useful.
        AddToCXXFLAGSIfSupported(myenv, "-Wno-defaulted-function-deleted")

        # SERVER-44856: Our windows builds complain about unused
        # exception parameters, but GCC and clang don't seem to do
        # that for us automatically. In the interest of making it more
        # likely to catch these errors early, add the (currently clang
        # only) flag that turns it on.
        AddToCXXFLAGSIfSupported(myenv, "-Wunused-exception-parameter")

        # TODO(SERVER-60151): Avoid the dilemma identified in
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100493. Unfortunately,
        # we don't have a more targeted warning suppression we can use
        # other than disabling all deprecation warnings. We will
        # revisit this once we are fully on C++20 and can commit the
        # C++20 style code.
        #
        # TODO(SERVER-60175): In fact we will want to explicitly opt
        # in to -Wdeprecated, since clang doesn't include it in -Wall.
        if get_option('cxx-std') == "20":
            AddToCXXFLAGSIfSupported(myenv, '-Wno-deprecated')

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

            context.Message('Checking if -Wnon-virtual-dtor works reasonably... ')
            ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
            context.Result(ret)
            return ret

        myenvClone = myenv.Clone()
        myenvClone.Append(CCFLAGS=[
            '$CCFLAGS_WERROR',
            '-Wnon-virtual-dtor',
        ], )
        conf = Configure(
            myenvClone,
            help=False,
            custom_tests={
                'CheckNonVirtualDtor': CheckNonVirtualDtor,
            },
        )
        if conf.CheckNonVirtualDtor():
            myenv.Append(CXXFLAGS=["-Wnon-virtual-dtor"])
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
                myenv.Append(LINKFLAGS=[
                    '-fstack-protector-strong',
                ], )
            elif AddToCCFLAGSIfSupported(myenv, '-fstack-protector-all'):
                myenv.Append(LINKFLAGS=[
                    '-fstack-protector-all',
                ], )

            if 'cfex' in selected_experimental_runtime_hardenings:
                myenv.Append(CFLAGS=[
                    "-fexceptions",
                ], )

            if 'stackclash' in selected_experimental_runtime_hardenings:
                AddToCCFLAGSIfSupported(myenv, "-fstack-clash-protection")

            if 'controlflow' in selected_experimental_runtime_hardenings:
                AddToCCFLAGSIfSupported(myenv, "-fcf-protection=full")

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
        message = """
        The --osx-version-min option is no longer supported.

        To specify a target minimum for Darwin platforms, please explicitly add the appropriate options
        to CCFLAGS and LINKFLAGS on the command line:

        macOS: scons CCFLAGS="-mmacosx-version-min=10.14" LINKFLAGS="-mmacosx-version-min=10.14" ..

        Note that MongoDB requires macOS 10.14 or later.
        """
        myenv.ConfError(textwrap.dedent(message))

    usingLibStdCxx = False
    if has_option('libc++'):
        if not myenv.ToolchainIs('clang'):
            myenv.FatalError('libc++ is currently only supported for clang')
        if AddToCXXFLAGSIfSupported(myenv, '-stdlib=libc++'):
            myenv.Append(LINKFLAGS=['-stdlib=libc++'])
        else:
            myenv.ConfError('libc++ requested, but compiler does not support -stdlib=libc++')
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

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'CheckLibStdCxx': CheckLibStdCxx,
            },
        )
        usingLibStdCxx = conf.CheckLibStdCxx()
        conf.Finish()

    if myenv.ToolchainIs('msvc'):
        if get_option('cxx-std') == "17":
            myenv.AppendUnique(CCFLAGS=['/std:c++17',
                                        '/Zc:lambda'])  # /Zc:lambda is implied by /std:c++20
        elif get_option('cxx-std') == "20":
            myenv.AppendUnique(CCFLAGS=['/std:c++20'])
    else:
        if get_option('cxx-std') == "17":
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++17'):
                myenv.ConfError('Compiler does not honor -std=c++17')
        elif get_option('cxx-std') == "20":
            if not AddToCXXFLAGSIfSupported(myenv, '-std=c++20'):
                myenv.ConfError('Compiler does not honor -std=c++20')

        if not AddToCFLAGSIfSupported(myenv, '-std=c11'):
            myenv.ConfError("C++17 mode selected for C++ files, but can't enable C11 for C files")

    if using_system_version_of_cxx_libraries():
        print('WARNING: System versions of C++ libraries must be compiled with C++17 support')

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

    def CheckCxx20(context):
        test_body = """
        #if __cplusplus < 202002L
        #error
        #endif
        #include <compare>
        [[maybe_unused]] constexpr auto spaceship_operator_is_a_cxx20_feature = 2 <=> 4;
        """

        context.Message('Checking for C++20... ')
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf = Configure(
        myenv,
        help=False,
        custom_tests={
            'CheckCxx17': CheckCxx17,
            'CheckCxx20': CheckCxx20,
        },
    )

    if get_option('cxx-std') == "17" and not conf.CheckCxx17():
        myenv.ConfError('C++17 support is required to build MongoDB')
    elif get_option('cxx-std') == "20" and not conf.CheckCxx20():
        myenv.ConfError('C++20 support was not detected')

    conf.Finish()

    # C11 memset_s - a secure memset
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

    conf = Configure(
        env,
        custom_tests={
            'CheckMemset_s': CheckMemset_s,
        },
    )
    if conf.CheckMemset_s():
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_MEMSET_S")

    if conf.CheckFunc('strnlen'):
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_STRNLEN")

    # Glibc 2.25+, OpenBSD 5.5+ and FreeBSD 11.0+ offer explicit_bzero, a secure way to zero memory
    if conf.CheckFunc('explicit_bzero'):
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_EXPLICIT_BZERO")

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

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'CheckModernLibStdCxx': CheckModernLibStdCxx,
            },
        )

        suppress_invalid = has_option("disable-minimum-compiler-version-enforcement")
        if not conf.CheckModernLibStdCxx() and not suppress_invalid:
            myenv.ConfError(
                "When using libstdc++, MongoDB requires libstdc++ from GCC 5.3.0 or newer")

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
        myenv.Append(CPPDEFINES=["_GLIBCXX_DEBUG"])

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

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'CheckWindowsSDKVersion': CheckWindowsSDKVersion,
            },
        )

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

    conf = Configure(
        myenv,
        help=False,
        custom_tests={
            'CheckPosixSystem': CheckPosixSystem,
        },
    )
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

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'CheckPosixMonotonicClock': CheckPosixMonotonicClock,
            },
        )
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

        using_asan = 'address' in sanitizer_list
        using_fsan = 'fuzzer' in sanitizer_list
        using_lsan = 'leak' in sanitizer_list
        using_tsan = 'thread' in sanitizer_list
        using_ubsan = 'undefined' in sanitizer_list
        using_msan = 'memory' in sanitizer_list

        if using_lsan:
            env.FatalError("Please use --sanitize=address instead of --sanitize=leak")

        if (using_asan
                or using_msan) and env['MONGO_ALLOCATOR'] in ['tcmalloc', 'tcmalloc-experimental']:
            # There are multiply defined symbols between the sanitizer and
            # our vendorized tcmalloc.
            env.FatalError("Cannot use --sanitize=address or --sanitize=memory with tcmalloc")

        if not myenv.ToolchainIs('clang') and using_msan:
            env.FatalError('Memory Sanitizer (MSan) is only supported with clang.')

        if using_fsan:

            def CheckForFuzzerCompilerSupport(context):

                test_body = """
                #include <stddef.h>
                #include <stdint.h>

                // fuzz_target.cc
                extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
                    return 0;
                }
                """

                context.Message("Checking if libfuzzer is supported by the compiler... ")

                context.env.AppendUnique(
                    LINKFLAGS=[
                        '-fprofile-instr-generate',
                        '-fcoverage-mapping',
                        '-fsanitize=fuzzer',
                    ],
                    CCFLAGS=[
                        '-fprofile-instr-generate',
                        '-fcoverage-mapping',
                    ],
                )

                ret = context.TryLink(textwrap.dedent(test_body), ".cpp")
                context.Result(ret)
                return ret

            confEnv = myenv.Clone()
            fuzzerConf = Configure(
                confEnv,
                help=False,
                custom_tests={
                    'CheckForFuzzerCompilerSupport': CheckForFuzzerCompilerSupport,
                },
            )
            if not fuzzerConf.CheckForFuzzerCompilerSupport():
                myenv.FatalError("libfuzzer is not supported by the compiler")
            fuzzerConf.Finish()

            # We can't include the fuzzer flag with the other sanitize flags
            # The libfuzzer library already has a main function, which will cause the dependencies check
            # to fail
            sanitizer_list.remove('fuzzer')
            sanitizer_list.append('fuzzer-no-link')
            # These flags are needed to generate a coverage report
            myenv.Append(LINKFLAGS=[
                '-fprofile-instr-generate',
                '-fcoverage-mapping',
            ], )
            myenv.Append(CCFLAGS=[
                '-fprofile-instr-generate',
                '-fcoverage-mapping',
            ], )

        sanitizer_option = '-fsanitize=' + ','.join(sanitizer_list)

        if AddToCCFLAGSIfSupported(myenv, sanitizer_option):
            myenv.Append(LINKFLAGS=[sanitizer_option])
            myenv.Append(CCFLAGS=['-fno-omit-frame-pointer'])
        else:
            myenv.ConfError('Failed to enable sanitizers with flag: {0}', sanitizer_option)

        myenv['SANITIZERS_ENABLED'] = sanitizer_list

        if has_option('sanitize-coverage') and using_fsan:
            sanitize_coverage_list = get_option('sanitize-coverage')
            sanitize_coverage_option = '-fsanitize-coverage=' + sanitize_coverage_list
            if AddToCCFLAGSIfSupported(myenv, sanitize_coverage_option):
                myenv.Append(LINKFLAGS=[sanitize_coverage_option])
            else:
                myenv.ConfError('Failed to enable -fsanitize-coverage with flag: {0}',
                                sanitize_coverage_option)

        denyfiles_map = {
            "address": myenv.File("#etc/asan.denylist"),
            "thread": myenv.File("#etc/tsan.denylist"),
            "undefined": myenv.File("#etc/ubsan.denylist"),
            "memory": myenv.File("#etc/msan.denylist"),
        }

        # Select those unique deny files that are associated with the
        # currently enabled sanitizers, but filter out those that are
        # zero length.
        denyfiles = {v for (k, v) in denyfiles_map.items() if k in sanitizer_list}
        denyfiles = [f for f in denyfiles if os.stat(f.path).st_size != 0]

        # Filter out any denylist options that the toolchain doesn't support.
        supportedDenyfiles = []
        denyfilesTestEnv = myenv.Clone()
        for denyfile in denyfiles:
            if AddToCCFLAGSIfSupported(denyfilesTestEnv, f"-fsanitize-blacklist={denyfile}"):
                supportedDenyfiles.append(denyfile)
        denyfilesTestEnv = None
        supportedDenyfiles = sorted(supportedDenyfiles)

        # If we ended up with any denyfiles after the above filters,
        # then expand them into compiler flag arguments, and use a
        # generator to return at command line expansion time so that
        # we can change the signature if the file contents change.
        if supportedDenyfiles:
            # Unconditionally using the full path can affect SCons cached builds, so we only do
            # this in cases where we know it's going to matter.
            denylist_options = [
                f"-fsanitize-blacklist={denyfile.path}" for denyfile in supportedDenyfiles
            ]

            if 'ICECC' in env and env['ICECC']:

                # Make these files available to remote icecream builds if requested.
                # These paths *must* be absolute to match the paths in the remote
                # toolchain archive. Local builds remain relative.
                local_denylist_options = denylist_options[:]
                denylist_options = [
                    f"-fsanitize-blacklist={denyfile.abspath}" for denyfile in supportedDenyfiles
                ]

                # Build a regex of all the regexes in the denylist
                # the regex in the denylist are a shell wildcard format
                # https://clang.llvm.org/docs/SanitizerSpecialCaseList.html#format
                # so a bit of massaging (* -> .*) to get a python regex.
                icecc_denylist_regexes = []
                for denyfile in supportedDenyfiles:
                    for line in denyfile.get_contents().decode('utf-8').split('\n'):
                        if line.strip().startswith('src:'):
                            regex_line = line.replace('src:', '').strip()
                            regex_line = re.escape(regex_line)
                            icecc_denylist_regexes += [regex_line.replace('\\*', ".*")]

                icecc_denylist_regex = re.compile('^(?:' + '|'.join(icecc_denylist_regexes) + ')$')

                def is_local_compile(env, target, source, for_signature):
                    return icecc_denylist_regex.match(str(source[0])) is not None

                env['ICECC_LOCAL_COMPILATION_FILTER'] = is_local_compile
                # If a sanitizer is in use with a denylist file, we have to ensure they get
                # added to the toolchain package that gets sent to the remote hosts so they
                # can be found by the remote compiler.
                env.Append(ICECC_CREATE_ENV_ADDFILES=supportedDenyfiles)

            if 'CCACHE' in env and env['CCACHE']:
                # Work around the fact that some versions of ccache either don't yet support
                # -fsanitize-blacklist at all or only support one instance of it. This will
                # work on any version of ccache because the point is only to ensure that the
                # resulting hash for any compiled object is guaranteed to take into account
                # the effect of any sanitizer denylist files used as part of the build.
                # TODO: This will no longer be required when the following pull requests/
                # issues have been merged and deployed.
                # https://github.com/ccache/ccache/pull/258
                # https://github.com/ccache/ccache/issues/318
                env.Append(CCACHE_EXTRAFILES=supportedDenyfiles)
                env['CCACHE_EXTRAFILES_USE_SOURCE_PATHS'] = True

            def CCSanitizerDenylistGenerator(source, target, env, for_signature):
                # TODO: SERVER-60915 use new conftest API
                if "conftest" in str(target[0]):
                    return ''

                # TODO: SERVER-64620 use scanner instead of for_signature
                if for_signature:
                    return [f.get_csig() for f in supportedDenyfiles]

                # Check if the denylist gets a match and if so it will be local
                # build and should use the non-abspath.
                # NOTE: in non icecream builds denylist_options becomes relative paths.
                if env.subst('$ICECC_LOCAL_COMPILATION_FILTER', target=target,
                             source=source) == 'True':
                    return local_denylist_options

                return denylist_options

            def LinkSanitizerDenylistGenerator(source, target, env, for_signature):
                # TODO: SERVER-60915 use new conftest API
                if "conftest" in str(target[0]):
                    return ''

                # TODO: SERVER-64620 use scanner instead of for_signature
                if for_signature:
                    return [f.get_csig() for f in supportedDenyfiles]

                return denylist_options

            myenv.AppendUnique(
                CC_SANITIZER_DENYLIST_GENERATOR=CCSanitizerDenylistGenerator,
                LINK_SANITIZER_DENYLIST_GENERATOR=LinkSanitizerDenylistGenerator,
                CCFLAGS="${CC_SANITIZER_DENYLIST_GENERATOR}",
                LINKFLAGS="${LINK_SANITIZER_DENYLIST_GENERATOR}",
            )

        symbolizer_option = ""
        if env.get('LLVM_SYMBOLIZER', False):
            llvm_symbolizer = env['LLVM_SYMBOLIZER']

            if not os.path.isabs(llvm_symbolizer):
                llvm_symbolizer = myenv.WhereIs(llvm_symbolizer)

            if not myenv.File(llvm_symbolizer).exists():
                myenv.FatalError(f"Symbolizer binary at path {llvm_symbolizer} does not exist")

            symbolizer_option = f":external_symbolizer_path=\"{llvm_symbolizer}\""

        elif using_asan or using_tsan or using_ubsan or using_msan:
            myenv.FatalError(
                "The address, thread, memory, and undefined behavior sanitizers require llvm-symbolizer for meaningful reports. Please set LLVM_SYMBOLIZER to the path to llvm-symbolizer in your SCons invocation"
            )

        if using_asan:
            # Unfortunately, abseil requires that we make these macros
            # (this, and THREAD_ and UNDEFINED_BEHAVIOR_ below) set,
            # because apparently it is too hard to query the running
            # compiler. We do this unconditionally because abseil is
            # basically pervasive via the 'base' library.
            myenv.AppendUnique(CPPDEFINES=['ADDRESS_SANITIZER'])
            # If anything is changed, added, or removed in either asan_options or
            # lsan_options, be sure to make the corresponding changes to the
            # appropriate build variants in etc/evergreen.yml
            asan_options_clear = [
                "detect_leaks=1",
                "check_initialization_order=true",
                "strict_init_order=true",
                "abort_on_error=1",
                "disable_coredump=0",
                "handle_abort=1",
                "strict_string_checks=true",
                "detect_invalid_pointer_pairs=1",
            ]
            asan_options = ":".join(asan_options_clear)
            lsan_options = f"report_objects=1:suppressions={myenv.File('#etc/lsan.suppressions').abspath}"
            env['ENV']['ASAN_OPTIONS'] = asan_options + symbolizer_option
            env['ENV']['LSAN_OPTIONS'] = lsan_options + symbolizer_option

        if using_msan:
            # Makes it easier to debug memory failures at the cost of some perf
            myenv.Append(CCFLAGS=['-fsanitize-memory-track-origins'])
            env['ENV']['MSAN_OPTIONS'] = symbolizer_option
        if using_tsan:

            if use_libunwind:
                # TODO: SERVER-48622
                #
                # See https://github.com/google/sanitizers/issues/943
                # for why we disallow combining TSAN with
                # libunwind. We could, atlernatively, have added logic
                # to automate the decision about whether to enable
                # libunwind based on whether TSAN is enabled, but that
                # logic is already complex, and it feels better to
                # make it explicit that using TSAN means you won't get
                # the benefits of libunwind. Fixing this is:
                env.FatalError(
                    "Cannot use libunwind with TSAN, please add --use-libunwind=off to your compile flags"
                )

            # If anything is changed, added, or removed in
            # tsan_options, be sure to make the corresponding changes
            # to the appropriate build variants in etc/evergreen.yml
            #
            # TODO SERVER-49121: die_after_fork=0 is a temporary
            # setting to allow tests to continue while we figure out
            # why we're running afoul of it.
            #
            # TODO SERVER-52413: report_thread_leaks=0 suppresses
            # reporting thread leaks, which we have because we don't
            # do a clean shutdown of the ServiceContext.
            #
            tsan_options = f"halt_on_error=1:report_thread_leaks=0:die_after_fork=0:suppressions={myenv.File('#etc/tsan.suppressions').abspath}"
            myenv['ENV']['TSAN_OPTIONS'] = tsan_options + symbolizer_option
            myenv.AppendUnique(CPPDEFINES=['THREAD_SANITIZER'])

        if using_ubsan:
            # By default, undefined behavior sanitizer doesn't stop on
            # the first error. Make it so. Newer versions of clang
            # have renamed the flag.
            # However, this flag cannot be included when using the fuzzer sanitizer
            # if we want to suppress errors to uncover new ones.
            if not using_fsan and not AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover"):
                AddToCCFLAGSIfSupported(myenv, "-fno-sanitize-recover=undefined")
            myenv.AppendUnique(CPPDEFINES=['UNDEFINED_BEHAVIOR_SANITIZER'])

            # If anything is changed, added, or removed in ubsan_options, be
            # sure to make the corresponding changes to the appropriate build
            # variants in etc/evergreen.yml
            ubsan_options = "print_stacktrace=1"
            myenv['ENV']['UBSAN_OPTIONS'] = ubsan_options + symbolizer_option

            # In dynamic builds, the `vptr` sanitizer check can
            # require additional LIBDEPS edges. That is very
            # inconvenient, because such builds can't use z,defs. The
            # result is a very fragile link graph, where refactoring
            # the link graph in one place can have surprising effects
            # in others. Instead, we just disable the `vptr` sanitizer
            # for dynamic builds. We tried some other approaches in
            # SERVER-49798 of adding a new LIBDEPS_TYPEINFO type, but
            # that didn't address the fundamental issue that the
            # correct link graph for a dynamic+ubsan build isn't the
            # same as the correct link graph for a regular dynamic
            # build.
            if link_model == "dynamic":
                if AddToCCFLAGSIfSupported(myenv, "-fno-sanitize=vptr"):
                    myenv.AppendUnique(LINKFLAGS=["-fno-sanitize=vptr"])

    if myenv.ToolchainIs('msvc') and optBuild != "off":
        # http://blogs.msdn.com/b/vcblog/archive/2013/09/11/introducing-gw-compiler-switch.aspx
        #
        myenv.Append(CCFLAGS=["/Gw", "/Gy"])
        myenv.Append(LINKFLAGS=["/OPT:REF"])

        # http://blogs.msdn.com/b/vcblog/archive/2014/03/25/linker-enhancements-in-visual-studio-2013-update-2-ctp2.aspx
        #
        myenv.Append(CCFLAGS=["/Zc:inline"])

    if myenv.ToolchainIs('gcc', 'clang'):
        # Usually, --gdb-index is too expensive in big static binaries, but for dynamic
        # builds it works well.
        if link_model.startswith("dynamic"):
            AddToLINKFLAGSIfSupported(myenv, '-Wl,--gdb-index')

        # Our build is already parallel.
        AddToLINKFLAGSIfSupported(myenv, '-Wl,--no-threads')

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
        if has_option('detect-odr-violations'):
            if myenv.ToolchainIs('clang') and usingLibStdCxx:
                env.FatalError(
                    'The --detect-odr-violations flag does not work with clang and libstdc++')
            if optBuild != "off":
                env.FatalError(
                    'The --detect-odr-violations flag is expected to only be reliable with --opt=off'
                )
            AddToLINKFLAGSIfSupported(myenv, '-Wl,--detect-odr-violations')

        # Disallow an executable stack. Also, issue a warning if any files are found that would
        # cause the stack to become executable if the noexecstack flag was not in play, so that we
        # can find them and fix them. We do this here after we check for ld.gold because the
        # --warn-execstack is currently only offered with gold.
        AddToLINKFLAGSIfSupported(myenv, "-Wl,-z,noexecstack")
        AddToLINKFLAGSIfSupported(myenv, "-Wl,--warn-execstack")

        # If possible with the current linker, mark relocations as read-only.
        AddToLINKFLAGSIfSupported(myenv, "-Wl,-z,relro")

        # As far as we know these flags only apply on posix-y systems,
        # and not on Darwin.
        if env.TargetOSIs("posix") and not env.TargetOSIs("darwin"):

            # Disable debug compression in both the assembler and linker
            # by default. If the user requested compression, only allow
            # the zlib-gabi form.
            debug_compress = get_option("debug-compress")

            # If a value was provided on the command line for --debug-compress, it should
            # inhibit the application of auto, so strip it out.
            if "auto" in debug_compress and len(debug_compress) > 1:
                debug_compress = debug_compress[1:]

            # Disallow saying --debug-compress=off --debug-compress=ld and similar
            if "off" in debug_compress and len(debug_compress) > 1:
                env.FatalError("Cannot combine 'off' for --debug-compress with other values")

            # Transform the 'auto' argument into a real value.
            if "auto" in debug_compress:
                debug_compress = []

                # We only automatically enable ld compression for
                # dynamic builds because it seems to use enormous
                # amounts of memory in static builds.
                if link_model.startswith("dynamic"):
                    debug_compress.append("ld")

            compress_type = "zlib-gabi"
            compress_flag = "compress-debug-sections"

            AddToCCFLAGSIfSupported(
                myenv,
                f"-Wa,--{compress_flag}={compress_type}"
                if "as" in debug_compress else f"-Wa,--no{compress_flag}",
            )

            # We shouldn't enable debug compression in the linker
            # (meaning our final binaries contain compressed debug
            # info) unless our local elf environment appears to at
            # least be aware of SHF_COMPRESSED. This seems like a
            # necessary precondition, but is it sufficient?
            #
            # https://gnu.wildebeest.org/blog/mjw/2016/01/13/elf-libelf-compressed-sections-and-elfutils/

            def CheckElfHForSHF_COMPRESSED(context):

                test_body = """
                #include <elf.h>
                #if !defined(SHF_COMPRESSED)
                #error
                #endif
                """

                context.Message('Checking elf.h for SHF_COMPRESSED... ')
                ret = context.TryCompile(textwrap.dedent(test_body), ".c")
                context.Result(ret)
                return ret

            conf = Configure(
                myenv,
                help=False,
                custom_tests={
                    'CheckElfHForSHF_COMPRESSED': CheckElfHForSHF_COMPRESSED,
                },
            )

            have_shf_compressed = conf.CheckElfHForSHF_COMPRESSED()
            conf.Finish()

            if have_shf_compressed and 'ld' in debug_compress:
                AddToLINKFLAGSIfSupported(
                    myenv,
                    f"-Wl,--{compress_flag}={compress_type}",
                )
            else:
                AddToLINKFLAGSIfSupported(
                    myenv,
                    f"-Wl,--{compress_flag}=none",
                )

        if "fnsi" in selected_experimental_optimizations:
            AddToCCFLAGSIfSupported(myenv, "-fno-semantic-interposition")

    # Avoid deduping symbols on OS X debug builds, as it takes a long time.
    if optBuild == "off" and myenv.ToolchainIs('clang') and env.TargetOSIs('darwin'):
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
                                "but selected compiler does not honor -flto")

            if myenv.TargetOSIs('darwin'):
                AddToLINKFLAGSIfSupported(myenv, '-Wl,-object_path_lto,${TARGET}.lto')

        else:
            myenv.ConfError("Don't know how to enable --lto on current toolchain")

    if get_option('runtime-hardening') == "on" and optBuild != "off":
        # Older glibc doesn't work well with _FORTIFY_SOURCE=2. Selecting 2.11 as the minimum was an
        # emperical decision, as that is the oldest non-broken glibc we seem to require. It is possible
        # that older glibc's work, but we aren't trying.
        #
        # https://gforge.inria.fr/tracker/?func=detail&group_id=131&atid=607&aid=14070
        # https://github.com/jedisct1/libsodium/issues/202
        def CheckForGlibcKnownToSupportFortify(context):
            test_body = """
            #include <features.h>
            #if !__GLIBC_PREREQ(2, 11)
            #error
            #endif
            """
            context.Message('Checking for glibc with non-broken _FORTIFY_SOURCE...')
            ret = context.TryCompile(textwrap.dedent(test_body), ".c")
            context.Result(ret)
            return ret

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'CheckForFortify': CheckForGlibcKnownToSupportFortify,
            },
        )

        # Fortify only possibly makes sense on POSIX systems, and we know that clang is not a valid
        # combination:
        #
        # http://lists.llvm.org/pipermail/cfe-dev/2015-November/045852.html
        #
        if env.TargetOSIs('posix') and not env.ToolchainIs('clang') and conf.CheckForFortify():
            conf.env.Append(CPPDEFINES=[
                ('_FORTIFY_SOURCE', 2),
            ], )

        myenv = conf.Finish()

    # Our build generally assumes that we have C11-compliant libc headers for
    # C++ source. On most systems, that will be the case. However, on systems
    # using glibc older than 2.18 (or other libc implementations that have
    # stubbornly refused to update), we need to add some preprocessor defines.
    #
    # See: https://sourceware.org/bugzilla/show_bug.cgi?id=15366
    #
    # These headers are only fully standards-compliant on POSIX platforms. Windows
    # in particular doesn't implement inttypes.h
    if env.TargetOSIs('posix'):

        def NeedStdCLimitMacros(context):
            test_body = """
            #undef __STDC_LIMIT_MACROS
            #include <stdint.h>
            #if defined(INT64_MAX)
            #  error
            #endif
            """
            context.Message('Checking whether to define __STDC_LIMIT_MACROS... ')
            ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
            context.Result(ret)
            return ret

        def NeedStdCConstantMacros(context):
            test_body = """
            #undef __STDC_CONSTANT_MACROS
            #include <stdint.h>
            #if defined(INTMAX_C)
            #  error
            #endif
            """
            context.Message('Checking whether to define __STDC_CONSTANT_MACROS... ')
            ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
            context.Result(ret)
            return ret

        def NeedStdCFormatMacros(context):
            test_body = """
            #undef __STDC_FORMAT_MACROS
            #include <inttypes.h>
            #if defined(PRIx64)
            #  error
            #endif
            """
            context.Message('Checking whether to define __STDC_FORMAT_MACROS... ')
            ret = context.TryCompile(textwrap.dedent(test_body), '.cpp')
            context.Result(ret)
            return ret

        conf = Configure(
            myenv,
            help=False,
            custom_tests={
                'NeedStdCLimitMacros': NeedStdCLimitMacros,
                'NeedStdCConstantMacros': NeedStdCConstantMacros,
                'NeedStdCFormatMacros': NeedStdCFormatMacros,
            },
        )

        conf.env.AppendUnique(CPPDEFINES=[
            '__STDC_LIMIT_MACROS' if conf.NeedStdCLimitMacros() else '',
            '__STDC_CONSTANT_MACROS' if conf.NeedStdCConstantMacros() else '',
            '__STDC_FORMAT_MACROS' if conf.NeedStdCFormatMacros() else '',
        ])

        myenv = conf.Finish()

    # We set this with GCC on x86 platforms to work around
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=43052
    if myenv.ToolchainIs('gcc') and (env['TARGET_ARCH'] in ['i386', 'x86_64']):
        if not 'builtin-memcmp' in selected_experimental_optimizations:
            AddToCCFLAGSIfSupported(myenv, '-fno-builtin-memcmp')

    # pthread_setname_np was added in GLIBC 2.12, and Solaris 11.3
    if posix_system:

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

        conf = Configure(
            myenv,
            custom_tests={
                'CheckPThreadSetNameNP': CheckPThreadSetNameNP,
            },
        )

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

    conf = Configure(
        myenv,
        custom_tests={
            'CheckBoostMinVersion': CheckBoostMinVersion,
        },
    )

    libdeps.setup_conftests(conf)

    ### --ssl checks
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
                advice = textwrap.dedent("""\
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
                        advice = textwrap.dedent("""\
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
                autoadd=True,
        ):
            maybeIssueDarwinSSLAdvice(conf.env)
            conf.env.ConfError("Couldn't find OpenSSL crypto.h header and library")

        def CheckLibSSL(context):
            res = SCons.Conftest.CheckLib(
                context,
                libs=[sslLibName],
                extra_libs=sslLinkDependencies,
                header='#include "openssl/ssl.h"',
                language="C",
                call="SSL_version(NULL);",
                autoadd=True,
            )
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
            """,
        ):
            conf.env.SetConfigHeaderDefine('MONGO_CONFIG_HAVE_FIPS_MODE_SET')

        if conf.CheckDeclaration(
                "d2i_ASN1_SEQUENCE_ANY",
                includes="""
                #include <openssl/asn1.h>
            """,
        ):
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

    # We require ssl by default unless the user has specified --ssl=off
    require_ssl = get_option("ssl") != "off"

    # The following platform checks setup both
    # ssl_provider for TLS implementation
    # and MONGO_CRYPTO for hashing support.
    #
    # MONGO_CRYPTO is always enabled regardless of --ssl=on/off
    # However, ssl_provider will be rewritten to 'none' if --ssl=off
    if conf.env.TargetOSIs('windows'):
        # SChannel on Windows
        ssl_provider = 'windows'
        conf.env.SetConfigHeaderDefine(
            "MONGO_CONFIG_SSL_PROVIDER",
            "MONGO_CONFIG_SSL_PROVIDER_WINDOWS",
        )
        conf.env.Append(MONGO_CRYPTO=["windows"])

    elif conf.env.TargetOSIs('darwin', 'macOS'):
        # SecureTransport on macOS
        ssl_provider = 'apple'
        conf.env.SetConfigHeaderDefine(
            "MONGO_CONFIG_SSL_PROVIDER",
            "MONGO_CONFIG_SSL_PROVIDER_APPLE",
        )
        conf.env.Append(MONGO_CRYPTO=["apple"])
        conf.env.AppendUnique(FRAMEWORKS=['CoreFoundation', 'Security'])

    elif require_ssl:
        checkOpenSSL(conf)
        # Working OpenSSL available, use it.
        conf.env.SetConfigHeaderDefine(
            "MONGO_CONFIG_SSL_PROVIDER",
            "MONGO_CONFIG_SSL_PROVIDER_OPENSSL",
        )
        conf.env.Append(MONGO_CRYPTO=["openssl"])
        ssl_provider = 'openssl'

    else:
        # If we don't need an SSL build, we can get by with TomCrypt.
        conf.env.Append(MONGO_CRYPTO=["tom"])

    if require_ssl:
        # Either crypto engine is native,
        # or it's OpenSSL and has been checked to be working.
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_SSL")
        print("Using SSL Provider: {0}".format(ssl_provider))
    else:
        ssl_provider = "none"

    def checkHTTPLib(required=False):
        # WinHTTP available on Windows
        if env.TargetOSIs("windows"):
            return True

        # libcurl on all other platforms
        if conf.CheckLibWithHeader(
                "curl",
            ["curl/curl.h"],
                "C",
                "curl_global_init(0);",
                autoadd=False,
        ):
            return True

        if required:
            env.ConfError("Could not find <curl/curl.h> and curl lib")

        return False

    if use_system_version_of_library("pcre2"):
        conf.FindSysLibDep("pcre2", ["pcre2-8"])
    else:
        conf.env.Prepend(CPPDEFINES=['PCRE2_STATIC'])

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

    if use_system_version_of_library("libunwind"):
        conf.FindSysLibDep("unwind", ["unwind"])

    if use_libunwind:
        if not conf.FindSysLibDep("lzma", ["lzma"]):
            myenv.ConfError("Cannot find system library 'lzma' required for use with libunwind")

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
        if not conf.CheckCXXHeader("wiredtiger.h"):
            myenv.ConfError("Cannot find wiredtiger headers")
        conf.FindSysLibDep("wiredtiger", ["wiredtiger"])

    conf.env.Append(CPPDEFINES=[
        "ABSL_FORCE_ALIGNED_ACCESS",
        "BOOST_ENABLE_ASSERT_DEBUG_HANDLER",
        # TODO: Ideally, we could not set this define in C++20
        # builds, but at least our current Xcode 12 doesn't offer
        # std::atomic_ref, so we cannot.
        "BOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF",
        "BOOST_LOG_NO_SHORTHAND_NAMES",
        "BOOST_LOG_USE_NATIVE_SYSLOG",
        "BOOST_LOG_WITHOUT_THREAD_ATTR",
        "BOOST_MATH_NO_LONG_DOUBLE_MATH_FUNCTIONS",
        "BOOST_SYSTEM_NO_DEPRECATED",
        "BOOST_THREAD_USES_DATETIME",
        ("BOOST_THREAD_VERSION", "5"),
    ])

    if link_model.startswith("dynamic") and not link_model == 'dynamic-sdk':
        conf.env.AppendUnique(CPPDEFINES=[
            "BOOST_LOG_DYN_LINK",
        ])

    if use_system_version_of_library("boost"):
        if not conf.CheckCXXHeader("boost/filesystem/operations.hpp"):
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
                    language='C++',
                )
    if posix_system:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_HEADER_UNISTD_H")
        conf.CheckLib('rt')
        conf.CheckLib('dl')

    if posix_monotonic_clock:
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_POSIX_MONOTONIC_CLOCK")

    if get_option('use-diagnostic-latches') == 'off':
        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_USE_RAW_LATCHES")

    if (conf.CheckCXXHeader("execinfo.h")
            and conf.CheckDeclaration('backtrace', includes='#include <execinfo.h>')
            and conf.CheckDeclaration('backtrace_symbols', includes='#include <execinfo.h>')
            and conf.CheckDeclaration('backtrace_symbols_fd', includes='#include <execinfo.h>')):

        conf.env.SetConfigHeaderDefine("MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE")

    conf.env["_HAVEPCAP"] = conf.CheckLib(["pcap", "wpcap"], autoadd=False)

    if env.TargetOSIs('solaris'):
        conf.CheckLib("nsl")

    conf.env['MONGO_BUILD_SASL_CLIENT'] = bool(has_option("use-sasl-client"))
    if conf.env['MONGO_BUILD_SASL_CLIENT'] and not conf.CheckLibWithHeader(
            "sasl2",
        ["stddef.h", "sasl/sasl.h"],
            "C",
            "sasl_version_info(0, 0, 0, 0, 0, 0);",
            autoadd=False,
    ):
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

        context.Message("Checking if std::atomic<{0}> works{1}... ".format(
            base_type, extra_message))

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

        context.Message(
            'Checking for extended alignment {0} for concurrency types... '.format(size))
        ret = context.TryCompile(textwrap.dedent(test_body), ".cpp")
        context.Result(ret)
        return ret

    conf.AddTest('CheckExtendedAlignment', CheckExtendedAlignment)

    # If we don't have a specialized search sequence for this
    # architecture, assume 64 byte cache lines, which is pretty
    # standard. If for some reason the compiler can't offer that, try
    # 32.
    default_alignment_search_sequence = [64, 32]

    # The following are the target architectures for which we have
    # some knowledge that they have larger cache line sizes. In
    # particular, POWER8 uses 128 byte lines and zSeries uses 256. We
    # start at the goal state, and work down until we find something
    # the compiler can actualy do for us.
    extended_alignment_search_sequence = {
        'ppc64le': [128, 64, 32],
        's390x': [256, 128, 64, 32],
    }

    for size in extended_alignment_search_sequence.get(env['TARGET_ARCH'],
                                                       default_alignment_search_sequence):
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

    mongoc_mode = get_option('use-system-mongo-c')
    conf.env['MONGO_HAVE_LIBMONGOC'] = False
    if mongoc_mode != 'off':
        if conf.CheckLibWithHeader(
            ["mongoc-1.0"],
            ["mongoc/mongoc.h"],
                "C",
                "mongoc_get_major_version();",
                autoadd=False,
        ):
            conf.env['MONGO_HAVE_LIBMONGOC'] = True
        if not conf.env['MONGO_HAVE_LIBMONGOC'] and mongoc_mode == 'on':
            myenv.ConfError("Failed to find the required C driver headers")
        if conf.env['MONGO_HAVE_LIBMONGOC'] and not conf.CheckMongoCMinVersion():
            myenv.ConfError("Version of mongoc is too old. Version 1.13+ required")

    # ask each module to configure itself and the build environment.
    moduleconfig.configure_modules(mongo_modules, conf)

    # Resolve --enable-free-mon
    if free_monitoring == "auto":
        if 'enterprise' not in conf.env['MONGO_MODULES']:
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

        outputIndex = next((idx for idx in [0, 1] if conf.CheckAltivecVbpermqOutput(idx)), None)
        if outputIndex is not None:
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_ALTIVEC_VEC_VBPERMQ_OUTPUT_INDEX",
                                           outputIndex)
        else:
            myenv.ConfError(
                "Running on ppc64le, but can't find a correct vec_vbpermq output index.  Compiler or platform not supported"
            )

    myenv = conf.Finish()

    if env['TARGET_ARCH'] == "aarch64":
        AddToCCFLAGSIfSupported(myenv, "-moutline-atomics")

    conf = Configure(myenv)
    usdt_enabled = get_option('enable-usdt-probes')
    usdt_provider = None
    if usdt_enabled in ('auto', 'on'):
        if env.TargetOSIs('linux'):
            if conf.CheckHeader('sys/sdt.h'):
                usdt_provider = 'SDT'
        # can put other OS targets here
        if usdt_enabled == 'on' and not usdt_provider:
            myenv.ConfError(
                "enable-usdt-probes flag was set to on, but no USDT provider could be found")
        elif usdt_provider:
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_USDT_ENABLED")
            conf.env.SetConfigHeaderDefine("MONGO_CONFIG_USDT_PROVIDER", usdt_provider)
    myenv = conf.Finish()

    return myenv


env = doConfigure(env)
env["NINJA_SYNTAX"] = "#site_scons/third_party/ninja_syntax.py"

if env.ToolchainIs("clang"):
    env["ICECC_COMPILER_TYPE"] = "clang"
elif env.ToolchainIs("gcc"):
    env["ICECC_COMPILER_TYPE"] = "gcc"

# Now that we are done with configure checks, enable ccache and
# icecream if requested.
if 'CCACHE' in env and env['CCACHE']:
    ccache = Tool('ccache')
    if not ccache.exists(env):
        env.FatalError(f"Failed to load ccache tool with CCACHE={env['CCACHE']}")
    ccache(env)
if 'ICECC' in env and env['ICECC']:
    env['ICECREAM_VERBOSE'] = env.Verbose()
    env['ICECREAM_TARGET_DIR'] = '$BUILD_ROOT/scons/icecream'

    # Posssibly multiple ninja files are in play, and there are cases where ninja will
    # use the wrong icecc run script, so we must create a unique script per ninja variant
    # for ninja to track separately. We will use the variant dir which contains the each
    # separate ninja builds meta files. This has to be under an additional flag then just
    # ninja disabled, because the run icecc script is generated under a context where ninja
    # is always disabled via the scons callback mechanism. The __NINJA_NO flag is intended
    # to differentiate this particular context.
    if env.get('__NINJA_NO') or get_option('ninja') != 'disabled':
        env['ICECREAM_RUN_SCRIPT_SUBPATH'] = '$VARIANT_DIR'

    icecream = Tool('icecream')
    if not icecream.exists(env):
        env.FatalError(f"Failed to load icecream tool with ICECC={env['ICECC']}")
    icecream(env)

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
# Capitalize on the weird way SCons handles arguments to determine if
# the user configured it or not. If not, it is under our control. Try
# to set some helpful defaults.
initial_num_jobs = env.GetOption('num_jobs')
altered_num_jobs = initial_num_jobs + 1
env.SetOption('num_jobs', altered_num_jobs)
cpu_count = psutil.cpu_count()
if env.GetOption('num_jobs') == altered_num_jobs:
    # psutil.cpu_count returns None when it can't determine the
    # number. This always fails on BSD's for example. If the user
    # didn't specify, and we can't determine for a parallel build, it
    # is better to make the user restart and be explicit, rather than
    # give them a very slow build.
    if cpu_count is None:
        if get_option("ninja") != "disabled":
            env.FatalError(
                "Cannot auto-determine the appropriate size for the Ninja local_job pool. Please regenerate with an explicit -j argument to SCons"
            )
        else:
            env.FatalError(
                "Cannot auto-determine the appropriate build parallelism on this platform. Please build with an explicit -j argument to SCons"
            )

    if 'ICECC' in env and env['ICECC'] and get_option("ninja") == "disabled":
        # If SCons is driving and we are using icecream, scale up the
        # number of jobs. The icerun integration will prevent us from
        # overloading the local system.
        env.SetOption('num_jobs', 8 * cpu_count)
    else:
        # Otherwise, either icecream isn't in play, so just use local
        # concurrency for SCons builds, or we are generating for
        # Ninja, in which case num_jobs controls the size of the local
        # pool. Scale that up to the number of local CPUs.
        env.SetOption('num_jobs', cpu_count)
else:
    if (not has_option('force-jobs') and ('ICECC' not in env or not env['ICECC'])
            and env.GetOption('num_jobs') > cpu_count):

        env.FatalError("ERROR: Icecream not enabled while using -j higher than available cpu's. " +
                       "Use --force-jobs to override.")

if (get_option('ninja') != "disabled" and ('ICECC' not in env or not env['ICECC'])
        and not has_option('force-jobs')):

    print(f"WARNING: Icecream not enabled - Ninja concurrency will be capped at {cpu_count} jobs " +
          "without regard to the -j value passed to it. " +
          "Generate your ninja file with --force-jobs to disable this behavior.")
    env['NINJA_MAX_JOBS'] = cpu_count

if get_option('ninja') != 'disabled':

    if 'ICECREAM_VERSION' in env and not env.get('CCACHE', None):
        if env['ICECREAM_VERSION'] < parse_version("1.2"):
            env.FatalError(
                "Use of ccache is mandatory with --ninja and icecream older than 1.2. You are running {}."
                .format(env['ICECREAM_VERSION']))

    ninja_builder = Tool("ninja")
    env["NINJA_BUILDDIR"] = env.Dir("$NINJA_BUILDDIR")
    ninja_builder.generate(env)

    ninjaConf = Configure(
        env,
        help=False,
        custom_tests={
            'CheckNinjaCompdbExpand': env.CheckNinjaCompdbExpand,
        },
    )
    env['NINJA_COMPDB_EXPAND'] = ninjaConf.CheckNinjaCompdbExpand()
    ninjaConf.Finish()

    # TODO: API for getting the sconscripts programmatically
    # exists upstream: https://github.com/SCons/scons/issues/3625
    def ninja_generate_deps(env, target, source, for_signature):
        dependencies = env.Flatten([
            'SConstruct',
            glob(os.path.join('src', '**', 'SConscript'), recursive=True),
            glob(os.path.join(os.path.expanduser('~/.scons/'), '**', '*.py'), recursive=True),
            glob(os.path.join('site_scons', '**', '*.py'), recursive=True),
            glob(os.path.join('buildscripts', '**', '*.py'), recursive=True),
            glob(os.path.join('src/third_party/scons-*', '**', '*.py'), recursive=True),
            glob(os.path.join('src/mongo/db/modules', '**', '*.py'), recursive=True),
        ])

        return dependencies

    env['NINJA_REGENERATE_DEPS'] = ninja_generate_deps

    if env.TargetOSIs("windows"):
        # This is a workaround on windows for SERVER-48691 where the line length
        # in response files is too long:
        # https://developercommunity.visualstudio.com/content/problem/441978/fatal-error-lnk1170-line-in-command-file-contains.html
        #
        # Ninja currently does not support
        # storing a newline in the ninja file, and therefore you can not
        # easily generate it to the response files. The only documented
        # way to get newlines into the response file is to use the $in_newline
        # variable in the rule.
        #
        # This workaround will move most of the object or lib links into the
        # inputs and then make the respone file consist of the inputs plus
        # whatever options are left in the original response content
        # more info can be found here:
        # https://github.com/ninja-build/ninja/pull/1223/files/e71bcceefb942f8355aab83ab447d702354ba272#r179526824
        # https://github.com/ninja-build/ninja/issues/1000

        # we are making a new special rule which will leverage
        # the $in_newline to get newlines into our response file
        env.NinjaRule(
            "WINLINK",
            "$env$WINLINK @$out.rsp",
            description="Linked $out",
            deps=None,
            pool="local_pool",
            use_depfile=False,
            use_response_file=True,
            response_file_content="$rspc $in_newline",
        )

        # Setup the response file content generation to use our workaround rule
        # for LINK commands.
        provider = env.NinjaGenResponseFileProvider(
            "WINLINK",
            "$LINK",
        )
        env.NinjaRuleMapping("${LINKCOM}", provider)
        env.NinjaRuleMapping(env["LINKCOM"], provider)

        # The workaround function will move some of the content from the rspc
        # variable into the nodes inputs. We only want to move build nodes because
        # inputs must be files, so we make sure the the option in the rspc
        # file starts with the build directory.
        def winlink_workaround(env, node, ninja_build):
            if ninja_build and 'rspc' in ninja_build["variables"]:

                rsp_content = []
                inputs = []
                for opt in ninja_build["variables"]["rspc"].split():

                    # if its a candidate to go in the inputs add it, else keep it in the non-newline
                    # rsp_content list
                    if opt.startswith(str(env.Dir("$BUILD_DIR"))) and opt != str(node):
                        inputs.append(opt)
                    else:
                        rsp_content.append(opt)

                ninja_build["variables"]["rspc"] = ' '.join(rsp_content)
                ninja_build["inputs"] += [
                    infile for infile in inputs if infile not in ninja_build["inputs"]
                ]

        # We apply the workaround to all Program nodes as they have potential
        # response files that have lines that are too long.
        # This will setup a callback function for a node
        # so that when its been processed, we can make some final adjustments before
        # its generated to the ninja file.
        def winlink_workaround_emitter(target, source, env):
            env.NinjaSetBuildNodeCallback(target[0], winlink_workaround)
            return target, source

        builder = env['BUILDERS']["Program"]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([base_emitter, winlink_workaround_emitter])
        builder.emitter = new_emitter

    # idlc.py has the ability to print its implicit dependencies
    # while generating. Ninja can consume these prints using the
    # deps=msvc method.
    env.AppendUnique(IDLCFLAGS=[
        "--write-dependencies-inline",
    ])
    env.NinjaRule(
        rule="IDLC",
        command="cmd /c $cmd" if env.TargetOSIs("windows") else "$cmd",
        description="Generated $out",
        deps="msvc",
        pool="local_pool",
    )

    def get_idlc_command(env, node, action, targets, sources, executor=None):
        _, variables, _ = env.NinjaGetGenericShellCommand(node, action, targets, sources,
                                                          executor=executor)
        variables["msvc_deps_prefix"] = "import file:"
        return "IDLC", variables, env.subst(env['IDLC']).split()

    env.NinjaRuleMapping("$IDLCCOM", get_idlc_command)
    env.NinjaRuleMapping(env["IDLCCOM"], get_idlc_command)

    # We can create empty files for FAKELIB in Ninja because it
    # does not care about content signatures. We have to
    # write_uuid_to_file for FAKELIB in SCons because SCons does.
    env.NinjaRule(
        rule="FAKELIB",
        command="cmd /c copy 1>NUL NUL $out" if env["PLATFORM"] == "win32" else "touch $out",
    )

    def fakelib_in_ninja(env, node):
        """Generates empty .a files"""
        return {
            "outputs": [node.get_path()],
            "rule": "FAKELIB",
            "implicit": [str(s) for s in node.sources],
        }

    env.NinjaRegisterFunctionHandler("write_uuid_to_file", fakelib_in_ninja)

    def ninja_test_list_builder(env, node):
        test_files = [test_file.path for test_file in env["MONGO_TEST_REGISTRY"][node.path]]
        files = ' '.join(test_files)
        return {
            "outputs": [node.get_path()],
            "rule": "TEST_LIST",
            "implicit": test_files,
            "variables": {"files": files, },
        }

    if env["PLATFORM"] == "win32":
        cmd = 'cmd.exe /c del "$out" && for %a in ($files) do (echo %a >> "$out")'
    else:
        cmd = 'rm -f "$out"; for i in $files; do echo "$$i" >> "$out"; done;'

    env.NinjaRule(
        rule="TEST_LIST",
        description="Compiled test list: $out",
        command=cmd,
    )
    env.NinjaRegisterFunctionHandler("test_list_builder_action", ninja_test_list_builder)

    env['NINJA_GENERATED_SOURCE_ALIAS_NAME'] = 'generated-sources'

if get_option('separate-debug') == "on" or env.TargetOSIs("windows"):

    # The current ninja builder can't handle --separate-debug on non-Windows platforms
    # like linux or macOS, because they depend on adding extra actions to the link step,
    # which cannot be translated into the ninja bulider.
    if not env.TargetOSIs("windows") and get_option('ninja') != 'disabled':
        env.FatalError("Cannot use --separate-debug with Ninja on non-Windows platforms.")

    separate_debug = Tool('separate_debug')
    if not separate_debug.exists(env):
        env.FatalError(
            'Cannot honor --separate-debug because the separate_debug.py Tool reported as nonexistent'
        )
    separate_debug(env)

# TODO: SERVER-68475
# temp fix for BF-25986, should be removed when better solution is found
if env['SPLIT_DWARF'] == "auto":
    env['SPLIT_DWARF'] = env.ToolchainIs('gcc') and not link_model == "dynamic"

if env['SPLIT_DWARF']:
    env.Tool('split_dwarf')

env["AUTO_ARCHIVE_TARBALL_SUFFIX"] = "tgz"

env["AIB_META_COMPONENT"] = "all"
env["AIB_BASE_COMPONENT"] = "common"
env["AIB_DEFAULT_COMPONENT"] = "mongodb"

env.Tool('auto_install_binaries')
env.Tool('auto_archive')

env.DeclareRoles(
    roles=[
        env.Role(name="base", ),
        env.Role(name="debug", ),
        env.Role(
            name="dev",
            dependencies=[
                "runtime",
            ],
        ),
        env.Role(name="meta", ),
        env.Role(
            name="runtime",
            dependencies=[
                # On windows, we want the runtime role to depend
                # on the debug role so that PDBs end in the
                # runtime package.
                "debug" if env.TargetOSIs('windows') else None,
            ],
            silent=True,
        ),
    ],
    base_role="base",
    meta_role="meta",
)


def _aib_debugdir(source, target, env, for_signature):
    for s in source:
        origin = getattr(s.attributes, "debug_file_for", None)
        oentry = env.Entry(origin)
        osuf = oentry.get_suffix()
        map_entry = env["AIB_SUFFIX_MAP"].get(osuf)
        if map_entry:
            return map_entry[0]
    env.FatalError("Unable to find debuginfo file in _aib_debugdir: (source='{}')".format(
        str(source)))


env["PREFIX_DEBUGDIR"] = _aib_debugdir

env.AddSuffixMapping({
    "$PROGSUFFIX": env.SuffixMap(
        directory="$PREFIX_BINDIR",
        default_role="runtime",
    ),

    "$SHLIBSUFFIX": env.SuffixMap(
        directory="$PREFIX_BINDIR" \
        if mongo_platform.get_running_os_name() == "windows" \
        else "$PREFIX_LIBDIR",
        default_role="runtime",
    ),

    ".debug": env.SuffixMap(
        directory="$PREFIX_DEBUGDIR",
        default_role="debug",
    ),

    ".dSYM": env.SuffixMap(
        directory="$PREFIX_DEBUGDIR",
        default_role="debug",
    ),

    ".pdb": env.SuffixMap(
        directory="$PREFIX_DEBUGDIR",
        default_role="debug",
    ),
})

env.AddPackageNameAlias(
    component="dist",
    role="runtime",
    name="mongodb-dist",
)

env.AddPackageNameAlias(
    component="dist",
    role="debug",
    name="mongodb-dist-debugsymbols",
)

env.AddPackageNameAlias(
    component="dist-test",
    role="runtime",
    name="mongodb-binaries",
)

env.AddPackageNameAlias(
    component="dist-test",
    role="debug",
    name="mongo-debugsymbols",
)

env.AddPackageNameAlias(
    component="dbtest",
    role="runtime",
    name="dbtest-binary",
)

env.AddPackageNameAlias(
    component="dbtest",
    role="debug",
    name="dbtest-debugsymbols",
)

env.AddPackageNameAlias(
    component="jstestshell",
    role="runtime",
    name="mongodb-jstestshell",
)

env.AddPackageNameAlias(
    component="jstestshell",
    role="debug",
    name="mongodb-jstestshell-debugsymbols",
)

env.AddPackageNameAlias(
    component="mongocryptd",
    role="runtime",
    name="mongodb-cryptd",
)

env.AddPackageNameAlias(
    component="mongocryptd",
    role="debug",
    name="mongodb-cryptd-debugsymbols",
)

env.AddPackageNameAlias(
    component="mh",
    role="runtime",
    # TODO: we should be able to move this to where the mqlrun binary is
    # defined when AIB correctly uses environments instead of hooking into
    # the first environment used.
    name="mh-binaries",
)

env.AddPackageNameAlias(
    component="mh",
    role="debug",
    # TODO: we should be able to move this to where the mqlrun binary is
    # defined when AIB correctly uses environments instead of hooking into
    # the first environment used.
    name="mh-debugsymbols",
)


def rpath_generator(env, source, target, for_signature):
    # If the PREFIX_LIBDIR has an absolute path, we will use that directly as
    # RPATH because that indicates the final install destination of the libraries.
    prefix_libdir = env.subst('$PREFIX_LIBDIR')
    if os.path.isabs(prefix_libdir):
        return ['$PREFIX_LIBDIR']

    # If the PREFIX_LIBDIR is not an absolute path, we will use a relative path
    # from the bin to the lib dir.
    lib_rel = os.path.relpath(prefix_libdir, env.subst('$PREFIX_BINDIR'))

    if env['PLATFORM'] == 'posix':\
        return [env.Literal(f"\\$$ORIGIN/{lib_rel}")]

    if env['PLATFORM'] == 'darwin':
        return [
            f"@loader_path/{lib_rel}",
        ]


env['RPATH_GENERATOR'] = rpath_generator

if env['PLATFORM'] == 'posix':
    env.AppendUnique(
        RPATH='$RPATH_GENERATOR',
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
        ])
elif env['PLATFORM'] == 'darwin':
    # The darwin case uses an adhoc implementation of RPATH for SCons
    # since SCons does not support RPATH directly for macOS:
    #   https://github.com/SCons/scons/issues/2127
    # so we setup RPATH and LINKFLAGS ourselves.
    env['RPATHPREFIX'] = '-Wl,-rpath,'
    env['RPATHSUFFIX'] = ''
    env['RPATH'] = '$RPATH_GENERATOR'
    env.AppendUnique(
        LINKFLAGS="${_concat(RPATHPREFIX, RPATH, RPATHSUFFIX, __env__)}",
        SHLINKFLAGS=[
            "-Wl,-install_name,@rpath/${TARGET.file}",
        ],
    )

env.Default(env.Alias("install-default"))

# Load the compilation_db tool. We want to do this after configure so we don't end up with
# compilation database entries for the configure tests, which is weird.
if get_option('ninja') == 'disabled':
    env.Tool("compilation_db")

incremental_link = Tool('incremental_link')
if incremental_link.exists(env):
    incremental_link(env)


# Resource Files are Windows specific
def env_windows_resource_file(env, path):
    if env.TargetOSIs('windows'):
        return [env.RES(path)]
    else:
        return []


env.AddMethod(env_windows_resource_file, 'WindowsResourceFile')

# --- lint ----

if get_option('lint-scope') == 'changed':
    patch_file = env.Command(
        target="$BUILD_DIR/current.git.patch",
        source=[env.WhereIs("git")],
        action="${SOURCES[0]} diff $GITDIFFFLAGS > $TARGET",
    )

    env.AlwaysBuild(patch_file)

    pylinters = env.Command(
        target="#lint-pylinters",
        source=[
            "buildscripts/pylinters.py",
            patch_file,
        ],
        action=
        "REVISION=$REVISION ENTERPRISE_REV=$ENTERPRISE_REV $PYTHON ${SOURCES[0]} lint-git-diff",
    )

    clang_format = env.Command(
        target="#lint-clang-format",
        source=[
            "buildscripts/clang_format.py",
            patch_file,
        ],
        action=
        "REVISION=$REVISION ENTERPRISE_REV=$ENTERPRISE_REV $PYTHON ${SOURCES[0]} lint-git-diff",
    )

    eslint = env.Command(
        target="#lint-eslint",
        source=[
            "buildscripts/eslint.py",
            patch_file,
        ],
        action=
        "REVISION=$REVISION ENTERPRISE_REV=$ENTERPRISE_REV $PYTHON ${SOURCES[0]} lint-git-diff",
    )

else:
    pylinters = env.Command(
        target="#lint-pylinters",
        source=[
            "buildscripts/pylinters.py",
        ],
        action="$PYTHON ${SOURCES[0]} lint-all",
    )

    clang_format = env.Command(
        target="#lint-clang-format",
        source=[
            "buildscripts/clang_format.py",
        ],
        action="$PYTHON ${SOURCES[0]} lint-all",
    )

    eslint = env.Command(
        target="#lint-eslint",
        source=[
            "buildscripts/eslint.py",
        ],
        action="$PYTHON ${SOURCES[0]} --dirmode lint jstests/ src/mongo",
    )

sconslinters = env.Command(
    target="#lint-sconslinters",
    source=[
        "buildscripts/pylinters.py",
    ],
    action="$PYTHON ${SOURCES[0]} lint-scons",
)

lint_py = env.Command(
    target="#lint-lint.py",
    source=["buildscripts/quickcpplint.py"],
    action="$PYTHON ${SOURCES[0]} lint",
)

lint_errorcodes = env.Command(
    target="#lint-errorcodes",
    source=["buildscripts/errorcodes.py"],
    action="$PYTHON ${SOURCES[0]} --quiet",
)

env.Alias("lint", [lint_py, eslint, clang_format, pylinters, sconslinters, lint_errorcodes])
env.Alias("lint-fast", [eslint, clang_format, pylinters, sconslinters, lint_errorcodes])
env.AlwaysBuild("lint")
env.AlwaysBuild("lint-fast")

#  ----  INSTALL -------


def getSystemInstallName():
    arch_name = env.subst('$MONGO_DISTARCH')

    # We need to make sure the directory names inside dist tarballs are permanently
    # consistent, even if the target OS name used in scons is different. Any differences
    # between the names used by env.TargetOSIs/env.GetTargetOSName should be added
    # to the translation dictionary below.
    os_name_translations = {
        'windows': 'win32',
        'macOS': 'macos',
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
                separators=(',', ': '),
            ))


env.AddDistSrcCallback(add_version_to_distsrc)

env['SERVER_DIST_BASENAME'] = env.subst('mongodb-%s-$MONGO_DISTNAME' % (getSystemInstallName()))
env['MH_DIST_BASENAME'] = 'mh'
if get_option('legacy-tarball') == 'true':
    if ('tar-dist' not in COMMAND_LINE_TARGETS and 'zip-dist' not in COMMAND_LINE_TARGETS
            and 'archive-dist' not in COMMAND_LINE_TARGETS):
        env.FatalError('option --legacy-tarball only valid with an archive-dist target')
    env['PREFIX'] = '$SERVER_DIST_BASENAME'

module_sconscripts = moduleconfig.get_module_sconscripts(mongo_modules)

# This generates a numeric representation of the version string so that
# you can easily compare versions of MongoDB without having to parse
# the version string.
#
# Examples:
# 5.1.1-123 =>        ['5', '1', '1', '123', None, None] =>          [5, 1, 2, -100]
# 5.1.1-rc2 =>        ['5', '1', '1', 'rc2', 'rc', '2'] =>           [5, 1, 1, -23]
# 5.1.1-rc2-123 =>    ['5', '1', '1', 'rc2-123', 'rc', '2'] =>       [5, 1, 1, -23]
# 5.1.0-alpha-123 =>  ['5', '1', '0', 'alpha-123', 'alpha', ''] =>   [5, 1, 0, -50]
# 5.1.0-alpha1-123 => ['5', '1', '0', 'alpha1-123', 'alpha', '1'] => [5, 1, 0, -49]
# 5.1.1 =>            ['5', '1', '1', '', None, None] =>             [5, 1, 1, 0]

version_parts = [
    x for x in re.match(r'^(\d+)\.(\d+)\.(\d+)-?((?:(rc|alpha)(\d?))?.*)?',
                        env['MONGO_VERSION']).groups()
]
version_extra = version_parts[3] if version_parts[3] else ""
if version_parts[4] == 'rc':
    version_parts[3] = int(version_parts[5]) + -25
elif version_parts[4] == 'alpha':
    if version_parts[5] == '':
        version_parts[3] = -50
    else:
        version_parts[3] = int(version_parts[5]) + -50
elif version_parts[3]:
    version_parts[2] = int(version_parts[2]) + 1
    version_parts[3] = -100
else:
    version_parts[3] = 0
version_parts = [int(x) for x in version_parts[:4]]

# The following symbols are exported for use in subordinate SConscript files.
# Ideally, the SConscript files would be purely declarative.  They would only
# import build environment objects, and would contain few or no conditional
# statements or branches.
#
# Currently, however, the SConscript files do need some predicates for
# conditional decision making that hasn't been moved up to this SConstruct file,
# and they are exported here, as well.
Export([
    'debugBuild',
    'endian',
    'free_monitoring',
    'get_option',
    'has_option',
    'http_client',
    'jsEngine',
    'module_sconscripts',
    'optBuild',
    'releaseBuild',
    'selected_experimental_optimizations',
    'serverJs',
    'ssl_provider',
    'use_libunwind',
    'use_system_libunwind',
    'use_system_version_of_library',
    'use_vendored_libunwind',
    'version_extra',
    'version_parts',
    'wiredtiger',
])


def injectMongoIncludePaths(thisEnv):
    thisEnv.AppendUnique(CPPPATH=['$BUILD_DIR'])


env.AddMethod(injectMongoIncludePaths, 'InjectMongoIncludePaths')


def injectModule(env, module, **kwargs):
    injector = env['MODULE_INJECTORS'].get(module)
    if injector:
        return injector(env, **kwargs)
    return env


env.AddMethod(injectModule, 'InjectModule')

if get_option('ninja') == 'disabled':
    compileCommands = env.CompilationDatabase('compile_commands.json')
    # Initialize generated-sources Alias as a placeholder so that it can be used as a
    # dependency for compileCommands. This Alias will be properly updated in other SConscripts.
    env.Requires(compileCommands, env.Alias("generated-sources"))
    compileDb = env.Alias("compiledb", compileCommands)

msvc_version = ""
if 'MSVC_VERSION' in env and env['MSVC_VERSION']:
    msvc_version = "--version " + env['MSVC_VERSION'] + " "

# Microsoft Visual Studio Project generation for code browsing
if get_option("ninja") == "disabled":
    vcxprojFile = env.Command(
        "mongodb.vcxproj",
        compileCommands,
        r"$PYTHON buildscripts\make_vcxproj.py " + msvc_version + "mongodb",
    )
    vcxproj = env.Alias("vcxproj", vcxprojFile)

# TODO: maybe make these work like the other archive- aliases
# even though they aren't piped through AIB?
distSrc = env.DistSrc("distsrc.tar", NINJA_SKIP=True)
env.NoCache(distSrc)
env.Alias("distsrc-tar", distSrc)

distSrcGzip = env.GZip(
    target="distsrc.tgz",
    source=[distSrc],
    NINJA_SKIP=True,
)
env.NoCache(distSrcGzip)
env.Alias("distsrc-tgz", distSrcGzip)

distSrcZip = env.DistSrc("distsrc.zip", NINJA_SKIP=True)
env.NoCache(distSrcZip)
env.Alias("distsrc-zip", distSrcZip)

env.Alias("distsrc", "distsrc-tgz")

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
                except (AttributeError):
                    pass
                env.NoCache(t)
            return target, source

        def addNoCacheEmitter(builder):
            origEmitter = builder.emitter
            if SCons.Util.is_Dict(origEmitter):
                for k, v in origEmitter:
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

# TODO: find a way to consolidate SConscript calls to one call in
# SConstruct so they all use variant_dir
env.SConscript(
    dirs=[
        'jstests',
    ],
    duplicate=False,
    exports=[
        'env',
    ],
)

# Critically, this approach is technically incorrect. While all MongoDB
# SConscript files use our add_option wrapper, builtin tools can
# access SCons's GetOption/AddOption methods directly, causing their options
# to not be validated by this block.
(_, leftover) = _parser.parse_args(sys.argv)
# leftover contains unrecognized options, including environment variables,and
# the argv[0]. If we only look at flags starting with --, and we skip the first
# leftover value (argv[0]), anything that remains is an invalid option
invalid_options = list(filter(lambda x: x.startswith("--"), leftover[1:]))
if len(invalid_options) > 0:
    # users frequently misspell "variables-files" (note two `s`s) as
    # "variable-files" or "variables-file". Detect and help them out.
    for opt in invalid_options:
        bad_var_file_opts = ["--variable-file", "--variables-file", "--variable-files"]
        if opt in bad_var_file_opts or any(
            [opt.startswith(f"{bad_opt}=") for bad_opt in bad_var_file_opts]):
            print(
                f"WARNING: You supplied the invalid parameter '{opt}' to SCons. Did you mean --variables-files (both words plural)?"
            )
    fatal_error(None, f"ERROR: unknown options supplied to scons: {invalid_options}")

# Declare the cache prune target
cachePrune = env.Command(
    target="#cache-prune",
    source=[
        "#buildscripts/scons_cache_prune.py",
    ],
    action=
    "$PYTHON ${SOURCES[0]} --cache-dir=${CACHE_DIR.abspath} --cache-size=${CACHE_SIZE} --prune-ratio=${CACHE_PRUNE_TARGET/100.00}",
    CACHE_DIR=env.Dir(cacheDir),
)

env.AlwaysBuild(cachePrune)

# Add a trivial Alias called `configure`. This makes it simple to run,
# or re-run, the SConscript reading and conf tests, but not build any
# real targets. This can be helpful when you are planning a dry-run
# build, or simply want to validate your changes to SConstruct, tools,
# and all the other setup that happens before we begin a real graph
# walk.
env.Alias('configure', None)

# We have finished all SConscripts and targets, so we can ask
# auto_install_binaries to finalize the installation setup.
env.FinalizeInstallDependencies()

# Create a install-all-meta alias that excludes unittests. This is most useful in
# static builds where the resource requirements of linking 100s of static unittest
# binaries is prohibitive.
candidate_nodes = set([
    str(gchild) for gchild in env.Flatten(
        [child.all_children() for child in env.Alias('install-all-meta')[0].all_children()])
])
names = [f'install-{env["AIB_META_COMPONENT"]}', 'install-tests', env["UNITTEST_ALIAS"]]
env.Alias('install-all-meta-but-not-unittests', [
    node for node in candidate_nodes if str(node) not in names
    and not str(node).startswith(tuple([prefix_name + '-' for prefix_name in names]))
])

# We don't want installing files to cause them to flow into the cache,
# since presumably we can re-install them from the origin if needed.
env.NoCache(env.FindInstalledFiles())

# Substitute environment variables in any build targets so that we can
# say, for instance:
#
# > scons --prefix=/foo/bar '$DESTDIR'
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

# Do any final checks the Libdeps linter may need to do once all
# SConscripts have been read but before building begins.
libdeps.LibdepLinter(env).final_checks()
libdeps.generate_libdeps_graph(env)
