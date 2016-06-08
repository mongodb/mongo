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

_splitDwarfFlag = '-gsplit-dwarf'

# Cribbed from Tool/cc.py and Tool/c++.py. It would be better if
# we could obtain this from SCons.
_CSuffixes = ['.c']
if not SCons.Util.case_sensitive_suffixes('.c', '.C'):
    _CSuffixes.append('.C')

_CXXSuffixes = ['.cpp', '.cc', '.cxx', '.c++', '.C++']
if SCons.Util.case_sensitive_suffixes('.c', '.C'):
    _CXXSuffixes.append('.C')

def _dwo_emitter(target, source, env):
    new_targets = []
    for t in target:
        base, ext = SCons.Util.splitext(str(t))
        if not any(ext == env[osuffix] for osuffix in ['OBJSUFFIX', 'SHOBJSUFFIX']):
            continue
        # TODO: Move 'dwo' into DWOSUFFIX so it can be customized? For
        # now, GCC doesn't let you control the output filename, so it
        # doesn't matter.
        dwotarget = (t.builder.target_factory or env.File)(base + ".dwo")
        new_targets.append(dwotarget)
    targets = target + new_targets
    return (targets, source)

def generate(env):
    suffixes = []
    if _splitDwarfFlag in env['CCFLAGS']:
        suffixes = _CSuffixes + _CXXSuffixes
    else:
        if _splitDwarfFlag in env['CFLAGS']:
            suffixes.extend(_CSuffixes)
        if _splitDwarfFlag in env['CXXFLAGS']:
            suffixes.extend(_CXXSuffixes)

    for object_builder in SCons.Tool.createObjBuilders(env):
        emitterdict = object_builder.builder.emitter
        for suffix in emitterdict.iterkeys():
            if not suffix in suffixes:
                continue
            base = emitterdict[suffix]
            emitterdict[suffix] = SCons.Builder.ListEmitter([
                base,
                _dwo_emitter,
            ])

def exists(env):
    return any(_splitDwarfFlag in env[f] for f in ['CCFLAGS', 'CFLAGS', 'CXXFLAGS'])
