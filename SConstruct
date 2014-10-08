# -*- mode: python; -*-
import re
import os

EnsureSConsVersion( 2, 0, 0 )

if not os.sys.platform == "win32":
    print ("SConstruct is only supported for Windows, use build_posix for other platforms")
    Exit(1)

AddOption("--enable-zlib", dest="zlib", type="string", nargs=1, action="store",
          help="Use zlib compression")

AddOption("--enable-snappy", dest="snappy", type="string", nargs=1, action="store",
          help="Use snappy compression")

env = Environment(
    CPPPATH = ["#/src/include/", "#/."],
    CFLAGS = ["/Z7"],
    LINKFLAGS = ["/DEBUG"],
)

useZlib = GetOption("zlib")
useSnappy = GetOption("snappy")
wtlibs = []

conf = Configure(env)
if not conf.CheckCHeader('stdlib.h'):
    print 'stdlib.h must be installed!'
    Exit(1)

if useZlib:
    conf.emv.Append(CPPPATH=[useZlib + "/include"])
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

env = conf.Finish()

def GenerateWiredTigerH(target, source, env):
    # Read the version information from the RELEASE_INFO file
    for l in open('build_posix/aclocal/version-set.m4'):
        if re.match(r'^VERSION_', l):
            exec(l)

    print VERSION_STRING

    replacements = {
        '@VERSION_MAJOR@' : VERSION_MAJOR,
        '@VERSION_MINOR@' : VERSION_MINOR,
        '@VERSION_PATCH@' : VERSION_PATCH,
        '@VERSION_STRING@' : VERSION_STRING,
        '@uintmax_t_decl@': "",
        '@uintptr_t_decl@': "",
        '@off_t_decl@' : 'typedef int64_t wt_off_t;',
        '@wiredtiger_includes_decl@': 
        """
#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>"
        """
        }

    wt = open("src/include/wiredtiger.in")
    out = open("wiredtiger.h", "w")
    for l in wt:
        lr = l
        for r in replacements.items():
            lr = lr.replace(r[0], str(r[1]))
        out.write(lr)

    wt.close()
    out.close()


#
# WiredTiger library
#
filelist = open("dist/filelist")
distFiles = filelist.readlines()
wtsources = [b.strip().replace("os_posix", "os_win")
             for b in distFiles
             if not b.startswith("#") and len(b) > 1]
filelist.close()

if useZlib:
    wtsources.append("ext/compressors/zlib/zlib_compress.c")

if useSnappy:
    wtsources.append("ext/compressors/snappy/snappy_compress.c")

#wtsources.append("wiredtiger.h")

env.Command('wiredtiger.h', 'src/include/wiredtiger.in', GenerateWiredTigerH)

wtlib = env.Library("wiredtiger", wtsources)

env.Program("wt", [
    "src/utilities/util_backup.c",
    "src/utilities/util_cpyright.c",
    "src/utilities/util_compact.c",
    "src/utilities/util_create.c",
    "src/utilities/util_drop.c",
    "src/utilities/util_dump.c",
    "src/utilities/util_getopt.c",
    "src/utilities/util_list.c",
    "src/utilities/util_load.c",
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
