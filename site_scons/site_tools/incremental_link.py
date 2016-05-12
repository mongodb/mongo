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

def _tag_as_precious(target, source, env):
    env.Precious(target)
    return target, source

def generate(env):
    builders = env['BUILDERS']
    for builder in ('Program', 'SharedLibrary', 'LoadableModule'):
        emitter = builders[builder].emitter
        builders[builder].emitter = SCons.Builder.ListEmitter([
            emitter,
            _tag_as_precious,
        ])

def exists(env):
    # By default, the windows linker is incremental, so unless
    # overridden in the environment with /INCREMENTAL:NO, the tool is
    # in play.
    if env.TargetOSIs('windows') and not "/INCREMENTAL:NO" in env['LINKFLAGS']:
        return True

    # On posix platforms, excluding darwin, we may have enabled
    # incremental linking. Check for the relevant flags.
    if env.TargetOSIs('posix') and \
       not env.TargetOSIs('darwin') and \
       "-fuse-ld=gold" in env['LINKFLAGS'] and \
       "-Wl,--incremental" in env['LINKFLAGS']:
        return True

    return False
