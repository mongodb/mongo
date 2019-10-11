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

icecream_version_min = '1.1rc2'


def generate(env):

    if not exists(env):
        return

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
        icecc_version = env.Dir('$BUILD_ROOT/scons/icecc').File(icecc_version_target_filename)

        # Make an isolated environment so that our setting of ICECC_VERSION in the environment
        # doesn't appear when executing icecc_create_env
        toolchain_env = env.Clone()
        if toolchain_env.ToolchainIs('clang'):
            toolchain = env.Command(
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
    else:
        env.AppendUnique(
            CCFLAGS=[
                '-fdirectives-only'
            ]
        )

    if 'ICECC_SCHEDULER' in env:
        env['ENV']['USE_SCHEDULER'] = env['ICECC_SCHEDULER']

    # Not all platforms have the readlink utility, so create our own
    # generator for that.
    def icecc_version_gen(target, source, env, for_signature):
        f = env.File('$ICECC_VERSION')
        if not f.islink():
            return f
        return env.File(os.path.realpath(f.abspath))
    env['ICECC_VERSION_GEN'] = icecc_version_gen

    def icecc_version_arch_gen(target, source, env, for_signature):
        if 'ICECC_VERSION_ARCH' in env:
            return "${ICECC_VERSION_ARCH}:"
        return str()
    env['ICECC_VERSION_ARCH_GEN'] = icecc_version_arch_gen

    # Make compile jobs flow through icecc
    env['CCCOM'] = '$( ICECC_VERSION=${ICECC_VERSION_ARCH_GEN}${ICECC_VERSION_GEN} $ICECC $) ' + env['CCCOM']
    env['CXXCOM'] = '$( ICECC_VERSION=${ICECC_VERSION_ARCH_GEN}${ICECC_VERSION_GEN} $ICECC $) ' + env['CXXCOM']
    env['SHCCCOM'] = '$( ICECC_VERSION=${ICECC_VERSION_ARCH_GEN}${ICECC_VERSION_GEN} $ICECC $) ' + env['SHCCCOM']
    env['SHCXXCOM'] = '$( ICECC_VERSION=${ICECC_VERSION_ARCH_GEN}${ICECC_VERSION_GEN} $ICECC $) ' + env['SHCXXCOM']

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
        needed_version = parse_version(icecream_version_min)
        if icecc_version >= needed_version:
            validated = True

    return validated
