# Copyright 2015 MongoDB Inc.
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
import subprocess

# TODO: Make a SUFF variable for the suffix to write to
# TODO: Prevent using abilink when -gsplit-dwarf is in play, since it doesn't work
# TODO: Make a variable for the md5sum utility (allow any hasher)
# TODO: Add an ABILINKCOM variable to the Action, so it can be silenced.

def _detect(env):
    try:
        abidw = env['ABIDW']
        if not abidw:
            return None
        return abidw
    except KeyError:
        pass

    return env.WhereIs('abidw')

def _add_emitter(builder):
    base_emitter = builder.emitter

    def new_emitter(target, source, env):
        new_targets = []
        for t in target:
            abidw = str(t) + ".abidw"
            abidw = (t.builder.target_factory or env.File)(abidw)
            new_targets.append(abidw)
            setattr(t.attributes, "abidw", abidw)
        targets = target + new_targets
        return (targets, source)

    new_emitter = SCons.Builder.ListEmitter([base_emitter, new_emitter])
    builder.emitter = new_emitter

def _add_scanner(builder):
    old_scanner = builder.target_scanner
    path_function = old_scanner.path_function

    def new_scanner(node, env, path):
        old_results = old_scanner(node, env, path)
        new_results = []
        for base in old_results:
            abidw = getattr(env.Entry(base).attributes, "abidw", None)
            new_results.append(abidw if abidw else base)
        return new_results

    builder.target_scanner = SCons.Scanner.Scanner(function=new_scanner, path_function=path_function)

def _add_action(builder):
    actions = builder.action
    builder.action = actions + SCons.Action.Action("$ABIDW $TARGET | md5sum > ${TARGET}.abidw")

def exists(env):
    result = _detect(env) != None
    return result

def generate(env):

    if not exists(env):
        return

    builder = env['BUILDERS']['SharedLibrary']
    _add_emitter(builder)
    _add_action(builder)
    _add_scanner(builder)
    _add_scanner(env['BUILDERS']['Program'])
    _add_scanner(env['BUILDERS']['LoadableModule'])
