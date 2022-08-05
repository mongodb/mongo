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

import os

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
        dwotarget = (t.builder.target_factory or env.File)(base + env['DWOSUFFIX'])
        new_targets.append(dwotarget)
    targets = target + new_targets
    return (targets, source)


def _dwp_emitter(target, source, env):
    if "conftest" not in str(target[0]) and env.get('SPLIT_DWARF_DWP_FILES'):

        # Check if the information regarding where the dwo files is located are stored in
        # the binary or a separate debug file.
        if len(target) > 1 and os.path.splitext(str(target[1]))[1] == env.get('SEPDBG_SUFFIX'):
            target_file = target[1]
        else:
            target_file = target[0]

        dwp_file = env.DWP(
            target=env.File(os.path.splitext(target_file.name)[0] + env['DWPSUFFIX']),
            source=target_file,
        )
        env.NoCache(dwp_file)

        if hasattr(env, "AutoInstall") and env.get("AIB_COMPONENT"):
            env.AutoInstall(
                "$PREFIX_BINDIR",
                dwp_file,
                AIB_COMPONENT=env.get("AIB_COMPONENT"),
                AIB_ROLE="debug",
                AIB_EXTRA_COMPONENTS=env.get("AIB_EXTRA_COMPONENTS", []),
            )

    return target, source


def options(opts):
    opts.AddVariables(
        ('DWP', "Path to dwp binary."),
        ("SPLIT_DWARF_DWP_FILES", "Set to enable DWP file creation from split dwarf."),
    )


def generate(env):

    # these suffixes are not adjustable to the underlying tools (gcc)
    # but we leave them adjustable in scons in case this changes in certain circumstances
    env['DWOSUFFIX'] = env.get('DWOSUFFIX', '.dwo')
    env['DWPSUFFIX'] = env.get('DWPSUFFIX', '.dwp')

    env.Append(CCFLAGS=[_splitDwarfFlag])

    for object_builder in SCons.Tool.createObjBuilders(env):
        emitterdict = object_builder.builder.emitter
        for suffix in emitterdict.keys():
            if not suffix in _CSuffixes + _CXXSuffixes:
                continue
            base = emitterdict[suffix]
            emitterdict[suffix] = SCons.Builder.ListEmitter([
                base,
                _dwo_emitter,
            ])

    for builder in ["Program"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([base_emitter, _dwp_emitter])
        builder.emitter = new_emitter

    env['DWP'] = env.get('DWP', 'dwp')
    env['BUILDERS']['DWP'] = SCons.Builder.Builder(
        action=SCons.Action.Action(
            "$DWP -e $SOURCE -o $TARGET",
            "Building dwp file from $SOURCE" if not env.Verbose() else "",
        ))


def exists(env):
    return any(_splitDwarfFlag in env[f] for f in ["CCFLAGS", "CFLAGS", "CXXFLAGS"])
