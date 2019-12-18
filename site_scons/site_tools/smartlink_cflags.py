# Copyright 2020 MongoDB Inc.
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

# This tool adjusts the LINKFLAGS and SHLINKFLAGS so that
# [SH]C[{C,CXX}]FLAGS are reiterated on the link line. This is
# important for flags like -mmacosx-version-min which has effects at
# both compile and link time. We use the CXX flags if the sources are
# C++.

from SCons.Tool.cxx import iscplusplus

def _smartlink_flags(source, target, env, for_signature):
    if iscplusplus(source):
        return '$CXXFLAGS $CCFLAGS $CXXLINKFLAGS'
    return '$CFLAGS $CCFLAGS $CLINKFLAGS'

def _smartshlink_flags(source, target, env, for_signature):
    if iscplusplus(source):
        return '$SHCXXFLAGS $SHCCFLAGS $SHCXXLINKFLAGS'
    return '$SHCFLAGS $SHCCFLAGS $SHCLINKFLAGS'


def exists(env):
    return True

def generate(env):

    if not exists(env):
        return

    env['SMARTLINKFLAGS'] = _smartlink_flags
    env['SMARTSHLINKFLAGS'] = _smartshlink_flags

    env.PrependUnique(
        LINKFLAGS=['$SMARTLINKFLAGS'],
        SHLINKFLAGS=['$SMARTSHLINKFLAGS'],
    )
