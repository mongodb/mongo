# -*- mode: python -*-

# scons file for MongoDB c++ client library and examples

import os
import sys

# options
AddOption("--extrapath",
          dest="extrapath",
          type="string",
          nargs=1,
          action="store",
          help="comma separated list of add'l paths  (--extrapath /opt/foo/,/foo) static linking")

AddOption("--prefix",
          dest="prefix",
          type="string",
          nargs=1,
          action="store",
          default="/usr/local",
          help="installation root")


# Stub out has_option so that we can use it in SConscript.client
def has_option(name):
    return False

# Stub out use_system_version_of_library so we can use it in SConscript.client
def use_system_version_of_library(name):
    return True

env = Environment(BUILD_DIR='#build',
                  CLIENT_ARCHIVE='${CLIENT_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
                  CLIENT_DIST_BASENAME='mongo-cxx-driver',
                  CLIENT_LICENSE='#LICENSE.txt',
                  CLIENT_SCONSTRUCT='#SConstruct',
                  MSVS_ARCH=None,
                  PYTHON=sys.executable,
                  PYSYSPLATFORM=os.sys.platform)

if env['PYSYSPLATFORM'] == 'linux3':
    env['PYSYSPLATFORM'] = 'linux2'
if 'freebsd' in env['PYSYSPLATFORM']:
    env['PYSYSPLATFORM'] = 'freebsd'

def addExtraLibs(s):
    for x in s.split(","):
        if os.path.exists(x):
            env.Append(CPPPATH=[x + "/include", x],
                       LIBPATH=[x + "/lib", x + "/lib64"])

if GetOption( "extrapath" ) is not None:
    addExtraLibs( GetOption( "extrapath" ) )

env.Prepend(CPPPATH=["$BUILD_DIR", "$BUILD_DIR/mongo"])
env.Append(CPPDEFINES=[ "_SCONS", "MONGO_EXPOSE_MACROS" ])

nix = False
linux = False
windows = False
darwin = False

if "darwin" == sys.platform:
    addExtraLibs( "/opt/local/" )
    nix = True
    darwin = True
elif sys.platform in ("linux2", "linux3"):
    nix = True
    linux = True
elif sys.platform == 'win32':
    windows = True

if windows:
    env['DIST_ARCHIVE_SUFFIX'] = '.zip'
    env.Append(CCFLAGS=['/EHsc', '/O2'])
else:
    env['DIST_ARCHIVE_SUFFIX'] = '.tgz'

if nix:
    env.Append(CCFLAGS=["-O3", "-pthread"])
if linux:
    env.Append(LINKFLAGS=["-pthread"])

boostLibs = ["thread", "filesystem", "system"]
conf = Configure(env)
for lib in boostLibs:
    if not conf.CheckLib(["boost_%s-mt" % lib, "boost_%s" % lib],
                         language="C++"):
        if not windows:
            Exit(1)

env['MONGO_BUILD_SASL_CLIENT'] = conf.CheckLibWithHeader(
    "sasl2", "sasl/sasl.h", "C", "sasl_version_info(0, 0, 0, 0, 0, 0);", autoadd=False)

if (conf.CheckCXXHeader( "execinfo.h" ) and
    conf.CheckDeclaration('backtrace', includes='#include <execinfo.h>') and
    conf.CheckDeclaration('backtrace_symbols', includes='#include <execinfo.h>') and
    conf.CheckDeclaration('backtrace_symbols_fd', includes='#include <execinfo.h>')):

    env.Append( CPPDEFINES=[ "MONGO_HAVE_EXECINFO_BACKTRACE" ] )

conf.Finish()

class InstallSetup:
    binaries = False
    libraries = False
    headers = False
installSetup = InstallSetup()

Export("env has_option use_system_version_of_library installSetup")
Export("nix linux windows darwin")
env.SConscript('src/SConscript.client', variant_dir='$BUILD_DIR', duplicate=False)

mongoclient = env.Alias('mongoclient', ['${LIBPREFIX}mongoclient${LIBSUFFIX}'])
env.Default(mongoclient)


# install
env.Alias("install", GetOption('prefix'))
