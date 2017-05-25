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


def generate(env):
    if not exists(env):
        return

    class ThinArchiveNode(SCons.Node.FS.File):
        def __init__(self, name, directory, fs):
            SCons.Node.FS.File.__init__(self, name, directory, fs)

        def get_contents(self):
            child_sigs = sorted([child.get_csig() for child in self.children()])
            return ''.join(child_sigs)

        def get_content_hash(self):
            return SCons.Util.MD5signature(self.get_contents())


    def _ThinArchiveNode(env, name, directory = None, create = 1):
        return env.fs._lookup(env.subst(name), directory, ThinArchiveNode, create)

    env.AddMethod(_ThinArchiveNode, 'ThinArchiveNode')

    def archive_target_factory(arg):
        return env.ThinArchiveNode(arg)

    env['BUILDERS']['StaticLibrary'].target_factory = archive_target_factory

    env['ARFLAGS'] = SCons.Util.CLVar([arflag if arflag != "rc" else "rcsTD" for arflag in env['ARFLAGS']])

    def noop_action(env, target, source):
        pass

    # Disable running ranlib, since we added 's' above
    env['RANLIBCOM'] = noop_action
    env['RANLIBCOMSTR'] = 'Skipping ranlib for thin archive $TARGET'
