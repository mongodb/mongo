# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

import SCons

_splitDwarfFlag = "-gsplit-dwarf"

# Cribbed from Tool/cc.py and Tool/c++.py. It would be better if
# we could obtain this from SCons.
_CSuffixes = [".c"]
if not SCons.Util.case_sensitive_suffixes(".c", ".C"):
    _CSuffixes.append(".C")

_CXXSuffixes = [".cpp", ".cc", ".cxx", ".c++", ".C++"]
if SCons.Util.case_sensitive_suffixes(".c", ".C"):
    _CXXSuffixes.append(".C")


def _dwo_emitter(target, source, env):
    new_targets = []
    for t in target:
        base, ext = SCons.Util.splitext(str(t))
        if not any(ext == env[osuffix] for osuffix in ["OBJSUFFIX", "SHOBJSUFFIX"]):
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
    if _splitDwarfFlag in env["CCFLAGS"]:
        suffixes = _CSuffixes + _CXXSuffixes
    else:
        if _splitDwarfFlag in env["CFLAGS"]:
            suffixes.extend(_CSuffixes)
        if _splitDwarfFlag in env["CXXFLAGS"]:
            suffixes.extend(_CXXSuffixes)

    for object_builder in SCons.Tool.createObjBuilders(env):
        emitterdict = object_builder.builder.emitter
        for suffix in emitterdict.keys():
            if not suffix in suffixes:
                continue
            base = emitterdict[suffix]
            emitterdict[suffix] = SCons.Builder.ListEmitter([base, _dwo_emitter,])


def exists(env):
    return any(_splitDwarfFlag in env[f] for f in ["CCFLAGS", "CFLAGS", "CXXFLAGS"])
