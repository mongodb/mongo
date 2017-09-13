# Copyright 2016 MongoDB Inc.
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

import re
import subprocess

def exists(env):
    if not 'AR' in env:
        return False

    ar = env.subst(env['AR'])
    if not ar:
        return False

    # If the user has done anything confusing with ARFLAGS, bail out. We want to find
    # an item in ARFLAGS of the exact form 'rc'.
    if not "rc" in env['ARFLAGS']:
        return False

    pipe = SCons.Action._subproc(env, SCons.Util.CLVar(ar) + ['--version'],
                                 stdin = 'devnull',
                                 stderr = 'devnull',
                                 stdout = subprocess.PIPE)
    if pipe.wait() != 0:
        return False

    isgnu = False
    for line in pipe.stdout:
        if isgnu:
            continue  # consume all data
        isgnu = re.search(r'^GNU ar', line)

    return bool(isgnu)

def _add_emitter(builder):
    base_emitter = builder.emitter

    def new_emitter(target, source, env):
        for t in target:
            setattr(t.attributes, "thin_archive", True)
        return (target, source)

    new_emitter = SCons.Builder.ListEmitter([base_emitter, new_emitter])
    builder.emitter = new_emitter

def _add_scanner(builder):
    old_scanner = builder.target_scanner
    path_function = old_scanner.path_function

    def new_scanner(node, env, path):
        old_results = old_scanner(node, env, path)
        new_results = []
        for base in old_results:
            new_results.append(base)
            if getattr(env.Entry(base).attributes, "thin_archive", None):
                new_results.extend(base.children())
        return new_results

    builder.target_scanner = SCons.Scanner.Scanner(function=new_scanner, path_function=path_function)

def generate(env):
    if not exists(env):
        return

    env['ARFLAGS'] = SCons.Util.CLVar([arflag if arflag != "rc" else "rcsTD" for arflag in env['ARFLAGS']])

    def noop_action(env, target, source):
        pass

    # Disable running ranlib, since we added 's' above
    env['RANLIBCOM'] = noop_action
    env['RANLIBCOMSTR'] = 'Skipping ranlib for thin archive $TARGET'

    builder = env['BUILDERS']['StaticLibrary']
    _add_emitter(builder)

    _add_scanner(env['BUILDERS']['SharedLibrary'])
    _add_scanner(env['BUILDERS']['LoadableModule'])
    _add_scanner(env['BUILDERS']['Program'])
