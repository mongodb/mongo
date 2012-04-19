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


env = Environment(BUILD_DIR='#build',
                  CLIENT_ARCHIVE='${CLIENT_DIST_BASENAME}${DIST_ARCHIVE_SUFFIX}',
                  CLIENT_DIST_BASENAME='mongo-cxx-driver',
                  CLIENT_LICENSE='#LICENSE.txt',
                  CLIENT_SCONSTRUCT='#SConstruct',
                  MSVS_ARCH=None,
                  PYTHON=sys.executable)

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
win = False

if "darwin" == sys.platform:
    addExtraLibs( "/opt/local/" )
    nix = True
elif sys.platform in ("linux2", "linux3"):
    nix = True
    linux = True
elif sys.platform == 'win32':
    win = True

if win:
    env['DIST_ARCHIVE_SUFFIX'] = '.zip'
    env.Append(CCFLAGS=['/EHsc', '/O2'])
else:
    env['DIST_ARCHIVE_SUFFIX'] = '.tgz'

if nix:
    env.Append(CCFLAGS=["-O3", "-pthread"])
if linux:
    env.Append(LINKFLAGS=["-Wl,--as-needed", "-Wl,-zdefs"])

boostLibs = ["thread", "filesystem", "system"]
conf = Configure(env)
for lib in boostLibs:
    if not conf.CheckLib(["boost_%s-mt" % lib, "boost_%s" % lib],
                         language="C++"):
        if not win:
            Exit(1)
conf.Finish()

clientEnv = env.Clone()
clientEnv['CPPDEFINES'].remove('MONGO_EXPOSE_MACROS')
clientEnv.Prepend(LIBS=['mongoclient'], LIBPATH=['.'])

Export("env clientEnv")
env.SConscript('src/SConscript.client', variant_dir='$BUILD_DIR', duplicate=False)

env.Default('${LIBPREFIX}mongoclient${LIBSUFFIX}')


# install
env.Alias("install", GetOption('prefix'))
