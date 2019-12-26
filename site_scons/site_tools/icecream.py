# Copyright 2017 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import SCons

import os
import re
import subprocess

from pkg_resources import parse_version

_icecream_version_min = parse_version('1.1rc2')
_ccache_nocpp2_version = parse_version('3.4.1')


# I'd prefer to use value here, but amazingly, its __str__ returns the
# *initial* value of the Value and not the built value, if
# available. That seems like a bug. In the meantime, make our own very
# sinmple Substition thing.
class _BoundSubstitution:
    def __init__(self, env, expression):
        self.env = env
        self.expression = expression
        self.result = None

    def __str__(self):
        if self.result is None:
            self.result = self.env.subst(self.expression)
        return self.result

def generate(env):

    if not exists(env):
        return

    # If we are going to load the ccache tool, but we haven't done so
    # yet, then explicitly do it now. We need the ccache tool to be in
    # place before we setup icecream because we need to do things a
    # little differently if ccache is in play. If you don't use the
    # TOOLS variable to configure your tools, you should explicitly
    # load the ccache tool before you load icecream.
    if 'ccache' in env['TOOLS'] and not 'CCACHE_VERSION' in env:
        env.Tool('ccache')
    ccache_enabled = ('CCACHE_VERSION' in env)

    # Absoluteify, so we can derive ICERUN
    env['ICECC'] = env.WhereIs('$ICECC')

    if not 'ICERUN' in env:
        env['ICERUN'] = env.File('$ICECC').File('icerun')

    # Absoluteify, for parity with ICECC
    env['ICERUN'] = env.WhereIs('$ICERUN')

    # We can't handle sanitizer blacklist files, so disable icecc then, and just flow through
    # icerun to prevent slamming the local system with a huge -j value.
    if any(
            f.startswith("-fsanitize-blacklist=") for fs in ['CCFLAGS', 'CFLAGS', 'CXXFLAGS']
            for f in env[fs]):
        env['ICECC'] = '$ICERUN'

    # Make CC and CXX absolute paths too. It is better for icecc.
    env['CC'] = env.WhereIs('$CC')
    env['CXX'] = env.WhereIs('$CXX')

    if 'ICECC_VERSION' in env:
        # TODO:
        #
        # If ICECC_VERSION is a file, we are done. If it is a file
        # URL, resolve it to a filesystem path. If it is a remote UTL,
        # then fetch it to somewhere under $BUILD_ROOT/scons/icecc
        # with its "correct" name (i.e. the md5 hash), and symlink it
        # to some other deterministic name to use as icecc_version.

        pass
    else:
        # Make a predictable name for the toolchain
        icecc_version_target_filename = env.subst('$CC$CXX').replace('/', '_')
        icecc_version_dir = env.Dir('$BUILD_ROOT/scons/icecc')
        icecc_version = icecc_version_dir.File(icecc_version_target_filename)

        # There is a weird ordering problem that occurs when the ninja generator
        # is enabled with icecream. Because the modules system runs configure
        # checks after the environment is setup and configure checks ignore our
        # --no-exec from the Ninja tool they try to create the icecc_env file.
        # But since the Ninja tool has reached into the internals of SCons to
        # disabled as much of it as possible SCons never creates this directory,
        # causing the icecc_create_env call to fail. So we explicitly
        # force creation of the directory now so it exists in all
        # circumstances.
        env.Execute(SCons.Defaults.Mkdir(icecc_version_dir))

        # Make an isolated environment so that our setting of ICECC_VERSION in the environment
        # doesn't appear when executing icecc_create_env
        toolchain_env = env.Clone()
        if toolchain_env.ToolchainIs('clang'):
            toolchain = toolchain_env.Command(
                target=icecc_version,
                source=[
                    '$ICECC_CREATE_ENV',
                    '$CC',
                    '$CXX'
                ],
                action=[
                    "${SOURCES[0]} --clang ${SOURCES[1].abspath} /bin/true $TARGET",
                ],
            )
        else:
            toolchain = toolchain_env.Command(
                target=icecc_version,
                source=[
                    '$ICECC_CREATE_ENV',
                    '$CC',
                    '$CXX'
                ],
                action=[
                    "${SOURCES[0]} --gcc ${SOURCES[1].abspath} ${SOURCES[2].abspath} $TARGET",
                ],
            )

        # Create an emitter that makes all of the targets depend on the
        # icecc_version_target (ensuring that we have read the link), which in turn
        # depends on the toolchain (ensuring that we have packaged it).
        def icecc_toolchain_dependency_emitter(target, source, env):
            env.Requires(target, toolchain)
            return target, source

        # Cribbed from Tool/cc.py and Tool/c++.py. It would be better if
        # we could obtain this from SCons.
        _CSuffixes = ['.c']
        if not SCons.Util.case_sensitive_suffixes('.c', '.C'):
            _CSuffixes.append('.C')

        _CXXSuffixes = ['.cpp', '.cc', '.cxx', '.c++', '.C++']
        if SCons.Util.case_sensitive_suffixes('.c', '.C'):
            _CXXSuffixes.append('.C')

        suffixes = _CSuffixes + _CXXSuffixes
        for object_builder in SCons.Tool.createObjBuilders(env):
            emitterdict = object_builder.builder.emitter
            for suffix in emitterdict.keys():
                if not suffix in suffixes:
                    continue
                base = emitterdict[suffix]
                emitterdict[suffix] = SCons.Builder.ListEmitter([
                    base,
                    icecc_toolchain_dependency_emitter
                ])

        # Add ICECC_VERSION to the environment, pointed at the generated
        # file so that we can expand it in the realpath expressions for
        # CXXCOM and friends below.
        env['ICECC_VERSION'] = icecc_version

    if env.ToolchainIs('clang'):
        env['ENV']['ICECC_CLANG_REMOTE_CPP'] = 1

        if ccache_enabled and env['CCACHE_VERSION'] >= _ccache_nocpp2_version:
            env.AppendUnique(
                CCFLAGS=[
                    '-frewrite-includes'
                ]
            )
            env['ENV']['CCACHE_NOCPP2'] = 1
    else:
        env.AppendUnique(
            CCFLAGS=[
                '-fdirectives-only'
            ]
        )
        if ccache_enabled:
            env['ENV']['CCACHE_NOCPP2'] = 1

    if 'ICECC_SCHEDULER' in env:
        env['ENV']['USE_SCHEDULER'] = env['ICECC_SCHEDULER']

    # Make sure it is a file node so that we can call `.abspath` on it
    # below. We must defer the abspath and realpath calls until after
    # the tool has completed and we have begun building, since we need
    # the real toolchain tarball to get created first on disk as part
    # of the DAG walk.
    env['ICECC_VERSION'] = env.File('$ICECC_VERSION')

    # Not all platforms have the readlink utility, so create our own
    # generator for that.
    def icecc_version_gen(target, source, env, for_signature):
        # Be careful here. If we are running with the ninja tool, many things
        # may have been monkey patched away. Rely only on `os`, not things
        # that may try to stat. The abspath appears to be ok.
        #
        # TODO: Another idea would be to eternally memoize lstat in
        # the ninja module, and then we could return to using a call
        # to islink on the ICECC_VERSION file.  Similarly, it would be
        # nice to be able to memoize away this call, but we should
        # think carefully about where to store the result of such
        # memoization.
        return os.path.realpath(env['ICECC_VERSION'].abspath)
    env['ICECC_VERSION_GEN'] = icecc_version_gen

    # Build up the string we will set in the environment to tell icecream
    # about the compiler package.
    icecc_version_string = '${ICECC_VERSION_GEN}'
    if 'ICECC_VERSION_ARCH' in env:
        icecc_version_string = '${ICECC_VERSION_ARCH}:' + icecc_version_string

    # Use our BoundSubstitition class to put ICECC_VERSION into
    # env['ENV'] with substitution in play. This lets us defer doing
    # the realpath in the generator above until after we have made the
    # tarball.
    env['ENV']['ICECC_VERSION'] = _BoundSubstitution(env, icecc_version_string)

    # If ccache is in play we actually want the icecc binary in the
    # CCACHE_PREFIX environment variable, not on the command line, per
    # the ccache documentation on compiler wrappers. Otherwise, just
    # put $ICECC on the command line. We wrap it in the magic "don't
    # consider this part of the build signature" sigils in the hope
    # that enabling and disabling icecream won't cause rebuilds. This
    # is unlikely to really work, since above we have maybe changed
    # compiler flags (things like -fdirectives-only), but we still try
    # to do the right thing.
    if ccache_enabled:
        env['ENV']['CCACHE_PREFIX'] = _BoundSubstitution(env, '$ICECC')
    else:
        icecc_string = '$( $ICECC $)'
        env['CCCOM'] = ' '.join([icecc_string, env['CCCOM']])
        env['CXXCOM'] = ' '.join([icecc_string, env['CXXCOM']])
        env['SHCCCOM'] = ' '.join([icecc_string, env['SHCCCOM']])
        env['SHCXXCOM'] = ' '.join([icecc_string, env['SHCXXCOM']])

    # Make link like jobs flow through icerun so we don't kill the
    # local machine.
    #
    # TODO: Should we somehow flow SPAWN or other universal shell launch through
    # ICERUN to avoid saturating the local machine, and build something like
    # ninja pools?
    env['ARCOM'] = '$( $ICERUN $) ' + env['ARCOM']
    env['LINKCOM'] = '$( $ICERUN $) ' + env['LINKCOM']
    env['SHLINKCOM'] = '$( $ICERUN $) ' + env['SHLINKCOM']

    # Uncomment these to debug your icecc integration
    # env['ENV']['ICECC_DEBUG'] = 'debug'
    # env['ENV']['ICECC_LOGFILE'] = 'icecc.log'


def exists(env):

    icecc = env.get('ICECC', False)
    if not icecc:
        return False
    icecc = env.WhereIs(icecc)
    if not icecc:
        return False

    pipe = SCons.Action._subproc(env,
                                 SCons.Util.CLVar(icecc) + ['--version'], stdin='devnull',
                                 stderr='devnull', stdout=subprocess.PIPE)

    if pipe.wait() != 0:
        return False

    validated = False
    for line in pipe.stdout:
        line = line.decode('utf-8')
        if validated:
            continue  # consume all data
        version_banner = re.search(r'^ICECC ', line)
        if not version_banner:
            continue
        icecc_version = re.split('ICECC (.+)', line)
        if len(icecc_version) < 2:
            continue
        icecc_version = parse_version(icecc_version[1])
        if icecc_version >= _icecream_version_min:
            validated = True

    return validated
