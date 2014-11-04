# -*- mode: python; -*-
import re
import os
import textwrap
import distutils.sysconfig

EnsureSConsVersion( 2, 0, 0 )

if not os.sys.platform == "win32":
    print ("SConstruct is only supported for Windows, use build_posix for other platforms")
    Exit(1)

AddOption("--with-berkeley-db", dest="bdb", type="string", nargs=1, action="store",
          help="Berkeley DB install path, ie, /usr/local")

AddOption("--enable-zlib", dest="zlib", type="string", nargs=1, action="store",
          help="Use zlib compression")

AddOption("--enable-snappy", dest="snappy", type="string", nargs=1, action="store",
          help="Use snappy compression")

AddOption("--enable-swig", dest="swig", type="string", nargs=1, action="store",
          help="Build python extension, specify location of swig.exe binary")

AddOption("--dynamic-crt", dest="dynamic-crt", action="store_true", default=False,
          help="Link with the MSVCRT DLL version")

env = Environment(
    CPPPATH = ["#/src/include/",
               "#/build_win",
               "#/test/windows",
               "#/.",
               distutils.sysconfig.get_python_inc()
           ],
    #CPPDEFINES = ["HAVE_DIAGNOSTIC", "HAVE_VERBOSE"],
    CFLAGS = [
        "/Z7", # Generate debugging symbols
        "/wd4090", # Ignore warning about mismatched const qualifiers
        "/wd4996", 
        "/W3", # Warning level 3
        "/we4013", # Error on undefined functions
        "/TC", # Compile as C code
        #"/Od", # Disable optimization
        "/Ob1", # inline expansion
        "/O2", # optimize for speed
        "/GF", # enable string pooling
        "/EHsc", # extern "C" does not throw
        #"/RTC1", # enable stack checks
        "/GS", # enable secrutiy checks
        "/Gy", # separate functions for linker
        "/Zc:wchar_t",
        "/Gd",
        "/MD" if GetOption("dynamic-crt") else "/MT",
        ],
    LINKFLAGS = [
        "/DEBUG", # Generate debug symbols
        "/INCREMENTAL:NO", # Disable incremental linking
        "/OPT:REF", # Remove dead code
        "/DYNAMICBASE",
        "/NXCOMPAT",
        ],
    LIBPATH=[ distutils.sysconfig.PREFIX + r"\libs"],
    tools=["default", "swig", "textfile"],
    SWIGFLAGS=['-python',
               "-threads",
               "-O",
               "-nodefaultctor",
               "-nodefaultdtor"
    ],
    SWIG=GetOption("swig")
)

useZlib = GetOption("zlib")
useSnappy = GetOption("snappy")
useBdb = GetOption("bdb")
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

if useBdb:
    conf.env.Append(CPPPATH=[useBdb+ "/include"])
    conf.env.Append(LIBPATH=[useBdb+ "/lib"])
    if not conf.CheckCHeader('db.h'):
        print 'db.h must be installed!'
        Exit(1)

env = conf.Finish()


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

env.Substfile(
    target='wiredtiger.h',
    source=[
        'src/include/wiredtiger.in',
    ],
    SUBST_DICT=replacements)

#
# WiredTiger library
#
filelistfile = r'dist\filelist.win'
filelist = open(filelistfile)
wtsources = [line.strip()
             for line in filelist
             if not line.startswith("#") and len(line) > 1]
filelist.close()

if useZlib:
    wtsources.append("ext/compressors/zlib/zlib_compress.c")

if useSnappy:
    wtsources.append("ext/compressors/snappy/snappy_compress.c")

wtlib = env.Library("wiredtiger", wtsources)

env.Depends(wtlib, [filelistfile, version_file])

env.Program("wt", [
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
    "src/utilities/util_rename.c",
    "src/utilities/util_salvage.c",
    "src/utilities/util_stat.c",
    "src/utilities/util_upgrade.c",
    "src/utilities/util_verbose.c",
    "src/utilities/util_verify.c",
    "src/utilities/util_write.c"],
    LIBS=[wtlib] + wtlibs)

if GetOption("swig"):
    env.SharedLibrary('_wiredtiger',
                      [ 'lang\python\wiredtiger.i'],
                      SHLIBSUFFIX=".pyd",
                      LIBS=[wtlib])

# Shim library of functions to emulate POSIX on Windows
shim = env.Library("window_shim",
        ["test/windows/windows_shim.c"])

env.Program("t_bloom",
    "test/bloom/test_bloom.c",
    LIBS=[wtlib])

#env.Program("t_checkpoint",
    #["test/checkpoint/checkpointer.c",
    #"test/checkpoint/test_checkpoint.c",
    #"test/checkpoint/workers.c"],
    #LIBS=[wtlib])

env.Program("t_huge",
    "test/huge/huge.c",
    LIBS=[wtlib])

#env.Program("t_fops",
    #["test/fops/file.c",
    #"test/fops/fops.c",
    #"test/fops/t.c"],
    #LIBS=[wtlib])

if useBdb:
    benv = env.Clone()

    benv.Append(CPPDEFINES=['BERKELEY_DB_PATH=\\"' + useBdb.replace("\\", "\\\\") + '\\"'])

    benv.Program("t_format",
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
         LIBS=[wtlib, shim, "libdb61"])

#env.Program("t_thread",
    #["test/thread/file.c",
    #"test/thread/rw.c",
    #"test/thread/stats.c",
    #"test/thread/t.c"],
    #LIBS=[wtlib])

#env.Program("t_salvage",
    #["test/salvage/salvage.c"],
    #LIBS=[wtlib])

env.Program("wtperf", [
    "bench/wtperf/config.c",
    "bench/wtperf/misc.c",
    "bench/wtperf/track.c",
    "bench/wtperf/wtperf.c",
    ],
    LIBS=[wtlib, shim] )

examples = [
    "ex_access",
    "ex_all",
    "ex_async",
    "ex_call_center",
    "ex_config",
    "ex_config_parse",
    "ex_cursor",
    "ex_data_source",
    "ex_extending",
    "ex_file",
    "ex_hello",
    "ex_log",
    "ex_pack",
    "ex_process",
    "ex_schema",
    "ex_scope",
    "ex_stat",
    "ex_thread",
    ]

for ex in examples:
    if(ex in ['ex_async', 'ex_thread']):
        env.Program(ex, "examples/c/" + ex + ".c", LIBS=[wtlib, shim])
    else:
        env.Program(ex, "examples/c/" + ex + ".c", LIBS=[wtlib])

