# -*- mode: python; -*-
import re
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import distutils.sysconfig

EnsureSConsVersion( 2, 0, 0 )

if not os.sys.platform == "win32":
    print ("SConstruct is only supported for Windows, use build_posix for other platforms")
    Exit(1)

# Command line options
#
AddOption("--dynamic-crt", dest="dynamic-crt", action="store_true", default=False,
          help="Link with the MSVCRT DLL version")

AddOption("--enable-attach", dest="attach", action="store_true", default=False,
          help="Configure for debugger attach on failure.")

AddOption("--enable-diagnostic", dest="diagnostic", action="store_true", default=False,
          help="Configure WiredTiger to perform various run-time diagnostic tests. DO NOT configure this option in production environments.")

AddOption("--enable-lz4", dest="lz4", type="string", nargs=1, action="store",
          help="Use LZ4 compression")

AddOption("--enable-python", dest="lang-python", type="string", nargs=1, action="store",
          help="Build Python extension, specify location of swig.exe binary")

AddOption("--enable-snappy", dest="snappy", type="string", nargs=1, action="store",
          help="Use snappy compression")

AddOption("--enable-tcmalloc", dest="tcmalloc", type="string", nargs=1, action="store",
          help="Use TCMalloc for memory allocation")

AddOption("--enable-verbose", dest="verbose", action="store_true", default=False,
          help="Configure WiredTiger to support the verbose configuration string to wiredtiger_open")

AddOption("--enable-zlib", dest="zlib", type="string", nargs=1, action="store",
          help="Use zlib compression")

AddOption("--prefix", dest="prefix", type="string", nargs=1, action="store", default="package",
          help="Install directory")

AddOption("--with-berkeley-db", dest="bdb", type="string", nargs=1, action="store",
          help="Berkeley DB install path, ie, /usr/local")

# Get the swig binary from the command line option since SCONS cannot find it automatically
#
swig_binary = GetOption("lang-python")

# Initialize environment
#
var = Variables()

var.Add('MSVC_USE_SCRIPT', 'Path to vcvars.bat to override SCons default VS tool search');

var.Add('CPPPATH', 'C Preprocessor include path', [
    "#/src/include/",
    "#/build_win",
    "#/test/windows",
    "#/.",
])

var.Add('CFLAGS', 'C Compiler Flags', [
    "/Z7", # Generate debugging symbols
    "/wd4090", # Ignore warning about mismatched const qualifiers
    "/wd4996", # Ignore deprecated functions
    "/W3", # Warning level 3
    #"/we4244", # Possible loss of data
    "/we4013", # Error on undefined functions
    #"/we4047", # Indirection differences in types
    #"/we4024", # Differences in parameter types
    #"/we4100", # Unreferenced local parameter
    "/TC", # Compile as C code
    #"/Od", # Disable optimization
    "/Ob1", # inline expansion
    "/O2", # optimize for speed
    "/GF", # enable string pooling
    "/EHsc", # extern "C" does not throw
    #"/RTC1", # enable stack checks
    "/GS", # enable security checks
    "/Gy", # separate functions for linker
    "/Zc:wchar_t",
    "/Gd",
    "/MD" if GetOption("dynamic-crt") else "/MT",
])

var.Add('LINKFLAGS', 'Linker Flags', [
    "/DEBUG", # Generate debug symbols
    "/INCREMENTAL:NO", # Disable incremental linking
    "/OPT:REF", # Remove dead code
    "/DYNAMICBASE",
    "/NXCOMPAT",
])

var.Add('TOOLS', 'SCons tools', [
    "default",
    "swig",
    "textfile"
])

var.Add('SWIG', 'SWIG binary location', swig_binary)

env = Environment(
    variables = var
)

env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1

useZlib = GetOption("zlib")
useSnappy = GetOption("snappy")
useLz4 = GetOption("lz4")
useBdb = GetOption("bdb")
useTcmalloc = GetOption("tcmalloc")
wtlibs = []

conf = Configure(env)
if not conf.CheckCHeader('stdlib.h'):
    print 'stdlib.h must be installed!'
    Exit(1)

if useZlib:
    conf.env.Append(CPPPATH=[useZlib + "/include"])
    conf.env.Append(LIBPATH=[useZlib + "/lib"])
    if conf.CheckCHeader('zlib.h'):
        conf.env.Append(CPPDEFINES=["HAVE_BUILTIN_EXTENSION_ZLIB"])
        wtlibs.append("zlib")
    else:
        print 'zlib.h must be installed!'
        Exit(1)

if useSnappy:
    conf.env.Append(CPPPATH=[useSnappy + "/include"])
    conf.env.Append(LIBPATH=[useSnappy + "/lib"])
    if conf.CheckCHeader('snappy-c.h'):
        conf.env.Append(CPPDEFINES=['HAVE_BUILTIN_EXTENSION_SNAPPY'])
        wtlibs.append("snappy")
    else:
        print 'snappy-c.h must be installed!'
        Exit(1)

if useLz4:
    conf.env.Append(CPPPATH=[useLz4 + "/include"])
    conf.env.Append(LIBPATH=[useLz4 + "/lib"])
    if conf.CheckCHeader('lz4.h'):
        conf.env.Append(CPPDEFINES=['HAVE_BUILTIN_EXTENSION_LZ4'])
        wtlibs.append("lz4")
    else:
        print 'lz4.h must be installed!'
        Exit(1)

if useBdb:
    conf.env.Append(CPPPATH=[useBdb+ "/include"])
    conf.env.Append(LIBPATH=[useBdb+ "/lib"])
    if not conf.CheckCHeader('db.h'):
        print 'db.h must be installed!'
        Exit(1)

if useTcmalloc:
    conf.env.Append(CPPPATH=[useTcmalloc + "/include"])
    conf.env.Append(LIBPATH=[useTcmalloc + "/lib"])
    if conf.CheckCHeader('gperftools/tcmalloc.h'):
        wtlibs.append("libtcmalloc_minimal")
        conf.env.Append(CPPDEFINES=['HAVE_LIBTCMALLOC'])
        conf.env.Append(CPPDEFINES=['HAVE_POSIX_MEMALIGN'])
    else:
        print 'tcmalloc.h must be installed!'
        Exit(1)

env = conf.Finish()

# Configure build environment variables
#
if GetOption("attach"):
    env.Append(CPPDEFINES = ["HAVE_ATTACH"])

if GetOption("diagnostic"):
    env.Append(CPPDEFINES = ["HAVE_DIAGNOSTIC"])

if GetOption("lang-python"):
    env.Append(LIBPATH=[distutils.sysconfig.PREFIX + r"\libs"])
    env.Append(CPPPATH=[distutils.sysconfig.get_python_inc()])

if GetOption("verbose"):
    env.Append(CPPDEFINES = ["HAVE_VERBOSE"])


# Build WiredTiger.h file
#
version_file = 'build_posix/aclocal/version-set.m4'

VERSION_MAJOR = None
VERSION_MINOR = None
VERSION_PATCH = None
VERSION_STRING = None

# Read the version information from the version-set.m4 file
for l in open(File(version_file).srcnode().abspath):
    if re.match(r'^VERSION_[A-Z]+', l):
        exec(l)

if (VERSION_MAJOR == None or
    VERSION_MINOR == None or
    VERSION_PATCH == None or
    VERSION_STRING == None):
    print "Failed to find version variables in " + version_file
    Exit(1)

wiredtiger_includes = """
        #include <sys/types.h>
        #include <stdarg.h>
        #include <stdbool.h>
        #include <stdint.h>
        #include <stdio.h>
    """
wiredtiger_includes = textwrap.dedent(wiredtiger_includes)
replacements = {
    '@VERSION_MAJOR@' : VERSION_MAJOR,
    '@VERSION_MINOR@' : VERSION_MINOR,
    '@VERSION_PATCH@' : VERSION_PATCH,
    '@VERSION_STRING@' : VERSION_STRING,
    '@uintmax_t_decl@': "",
    '@uintptr_t_decl@': "",
    '@off_t_decl@' : 'typedef int64_t wt_off_t;',
    '@wiredtiger_includes_decl@': wiredtiger_includes
}

wtheader = env.Substfile(
    target='wiredtiger.h',
    source=[
        'src/include/wiredtiger.in',
    ],
    SUBST_DICT=replacements)

#
# WiredTiger library
#
# Map WiredTiger build conditions: any conditions that appear in WiredTiger's
# dist/filelist must appear here, and if the value is true, those files will be
# included.
#
condition_map = {
    'POSIX_HOST' : env['PLATFORM'] == 'posix',
    'POWERPC_HOST' : False,
    'WINDOWS_HOST' : env['PLATFORM'] == 'win32',
}

def filtered_filelist(f):
    for line in f:
        file_cond = line.split()
        if line.startswith("#") or len(file_cond) == 0:
            continue
        if len(file_cond) == 1 or condition_map[file_cond[1]]:
            yield file_cond[0]

filelistfile = r'dist/filelist'
wtsources = list(filtered_filelist(open(filelistfile)))

if useZlib:
    wtsources.append("ext/compressors/zlib/zlib_compress.c")

if useSnappy:
    wtsources.append("ext/compressors/snappy/snappy_compress.c")

if useLz4:
    wtsources.append("ext/compressors/lz4/lz4_compress.c")

wt_objs = [env.Object(a) for a in wtsources]

# Static Library - libwiredtiger.lib
#
wtlib = env.Library(
    target="libwiredtiger",
    source=wt_objs, LIBS=wtlibs)

env.Depends(wtlib, [filelistfile, version_file])

# Dynamically Loaded Library - wiredtiger.dll
#
wtdll = env.SharedLibrary(
    target="wiredtiger",
    source=wt_objs + ['build_win/wiredtiger.def'], LIBS=wtlibs)

env.Depends(wtdll, [filelistfile, version_file])

Default(wtlib, wtdll)

wtbin = env.Program("wt", [
    "src/utilities/util_backup.c",
    "src/utilities/util_cpyright.c",
    "src/utilities/util_compact.c",
    "src/utilities/util_create.c",
    "src/utilities/util_drop.c",
    "src/utilities/util_dump.c",
    "src/utilities/util_list.c",
    "src/utilities/util_load.c",
    "src/utilities/util_load_json.c",
    "src/utilities/util_loadtext.c",
    "src/utilities/util_main.c",
    "src/utilities/util_misc.c",
    "src/utilities/util_printlog.c",
    "src/utilities/util_read.c",
    "src/utilities/util_rebalance.c",
    "src/utilities/util_rename.c",
    "src/utilities/util_salvage.c",
    "src/utilities/util_stat.c",
    "src/utilities/util_upgrade.c",
    "src/utilities/util_verbose.c",
    "src/utilities/util_verify.c",
    "src/utilities/util_write.c"],
    LIBS=[wtlib] + wtlibs)

Default(wtbin)

# Python SWIG wrapper for WiredTiger
if GetOption("lang-python"):
    # Check that this version of python is 64-bit
    #
    if sys.maxsize < 2**32:
        print "The Python Interpreter must be 64-bit in order to build the python bindings"
        Exit(1)

    pythonEnv = env.Clone()
    pythonEnv.Append(SWIGFLAGS=[
            "-python",
            "-threads",
            "-O",
            "-nodefaultctor",
            "-nodefaultdtor",
            ])

    swiglib = pythonEnv.SharedLibrary('_wiredtiger',
                      [ 'lang\python\wiredtiger.i'],
                      SHLIBSUFFIX=".pyd",
                      LIBS=[wtlib] + wtlibs)

    copySwig = pythonEnv.Command(
        'lang/python/wiredtiger/__init__.py',
        'lang/python/wiredtiger.py',
        Copy('$TARGET', '$SOURCE'))
    pythonEnv.Depends(copySwig, swiglib)

    swiginstall = pythonEnv.Install('lang/python/wiredtiger/', swiglib)

    Default(swiginstall, copySwig)

# Shim library of functions to emulate POSIX on Windows
shim = env.Library("window_shim",
        ["test/windows/windows_shim.c"])



examples = [
    "ex_access",
    "ex_all",
    "ex_async",
    "ex_call_center",
    "ex_config_parse",
    "ex_cursor",
    "ex_data_source",
    "ex_encrypt",
    "ex_extending",
    "ex_file_system",
    "ex_hello",
    "ex_log",
    "ex_pack",
    "ex_process",
    "ex_schema",
    "ex_scope",
    "ex_stat",
    "ex_thread",
    ]

# WiredTiger Smoke Test support
# Runs each test in a custom temporary directory
def run_smoke_test(x):
    print "Running Smoke Test: " + x

    # Make temp dir
    temp_dir = tempfile.mkdtemp(prefix="wt_home")

    try:
        # Set WT_HOME environment variable for test
        os.environ["WIREDTIGER_HOME"] = temp_dir

        # Run the test
        ret = subprocess.call(x);
        if( ret != 0):
            sys.stderr.write("Bad exit code %d\n" % (ret))
            raise Exception()

    finally:
        # Clean directory
        #
        shutil.rmtree(temp_dir)

def builder_smoke_test(target, source, env):
    run_smoke_test(source[0].abspath)
    return None

env.Append(BUILDERS={'SmokeTest' : Builder(action = builder_smoke_test)})

#Build the tests and setup the "scons test" target

testutil = env.Library('testutil',
            [
                'test/utility/misc.c',
                'test/utility/parse_opts.c'
            ])

#Don't test bloom on Windows, its broken
t = env.Program("t_bloom",
    "test/bloom/test_bloom.c",
    LIBS=[wtlib, testutil] + wtlibs)
#env.Alias("check", env.SmokeTest(t))
Default(t)

#env.Program("t_checkpoint",
    #["test/checkpoint/checkpointer.c",
    #"test/checkpoint/test_checkpoint.c",
    #"test/checkpoint/workers.c"],
    #LIBS=[wtlib])

t = env.Program("t_huge",
    "test/huge/huge.c",
    LIBS=[wtlib] + wtlibs)

#t = env.Program("t_recovery",
#    "test/recovery/recovery.c",
#    LIBS=[wtlib] + wtlibs)
#Default(t)

t = env.Program("t_fops",
    ["test/fops/file.c",
    "test/fops/fops.c",
    "test/fops/t.c"],
    LIBS=[wtlib, shim, testutil] + wtlibs)
env.Append(CPPPATH=["test/utility"])
env.Alias("check", env.SmokeTest(t))
Default(t)

if useBdb:
    benv = env.Clone()

    benv.Append(CPPDEFINES=['BERKELEY_DB_PATH=\\"' + useBdb.replace("\\", "\\\\") + '\\"'])

    t = benv.Program("t_format",
        ["test/format/backup.c",
        "test/format/bdb.c",
        "test/format/bulk.c",
        "test/format/compact.c",
        "test/format/config.c",
        "test/format/ops.c",
        "test/format/salvage.c",
        "test/format/t.c",
        "test/format/util.c",
        "test/format/wts.c"],
         LIBS=[wtlib, shim, "libdb61"] + wtlibs)
    env.Alias("test", env.SmokeTest(t))
    Default(t)

#env.Program("t_thread",
    #["test/thread/file.c",
    #"test/thread/rw.c",
    #"test/thread/stats.c",
    #"test/thread/t.c"],
    #LIBS=[wtlib])

#env.Program("t_salvage",
    #["test/salvage/salvage.c"],
    #LIBS=[wtlib])

t = env.Program("wtperf", [
    "bench/wtperf/config.c",
    "bench/wtperf/idle_table_cycle.c",
    "bench/wtperf/misc.c",
    "bench/wtperf/track.c",
    "bench/wtperf/wtperf.c",
    "bench/wtperf/wtperf_throttle.c",
    "bench/wtperf/wtperf_truncate.c",
    ],
    LIBS=[wtlib, shim]  + wtlibs)
Default(t)

#Build the Examples
for ex in examples:
    if(ex in ['ex_all', 'ex_async', 'ex_encrypt', 'ex_file_system' , 'ex_thread']):
        exp = env.Program(ex, "examples/c/" + ex + ".c", LIBS=[wtlib, shim] + wtlibs)
        Default(exp)
        env.Alias("check", env.SmokeTest(exp))
    else:
        exp = env.Program(ex, "examples/c/" + ex + ".c", LIBS=[wtdll[1]] + wtlibs)
        Default(exp)
        if not ex == 'ex_log':
            env.Alias("check", env.SmokeTest(exp))

# Install Target
#
prefix = GetOption("prefix")
env.Alias("install", env.Install(os.path.join(prefix, "bin"), wtbin))
env.Alias("install", env.Install(os.path.join(prefix, "bin"), wtdll[0])) # Just the dll
env.Alias("install", env.Install(os.path.join(prefix, "include"), wtheader))
env.Alias("install", env.Install(os.path.join(prefix, "lib"), wtdll[1])) # Just the import lib
env.Alias("install", env.Install(os.path.join(prefix, "lib"), wtlib))
