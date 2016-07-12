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

def _detect(env):
    try:
        abidw = env['ABIDW']
        if not abidw:
            return None
        return abidw
    except KeyError:
        pass

    return env.WhereIs('abidw')

def generate(env):

    class AbilinkNode(SCons.Node.FS.File):
        def __init__(self, name, directory, fs):
            SCons.Node.FS.File.__init__(self, name, directory, fs)

        def get_contents(self):
            if not self.rexists():
                return ''

            fname = self.rfile().abspath
            contents = None

            try:
                # TODO: If there were python bindings for libabigail, we
                # could avoid the shell out (and probably be faster, as we
                # could get exactly the information we want).
                contents = subprocess.check_output([env.subst('$ABIDW'), fname])
            except subprocess.CalledProcessError, e:
                # ABIDW sometimes fails. In that case, log an error
                # and fall back to the normal contents
                print "WARNING: ABIDW failed for target %s, please file a bug report" % fname
                try:
                    contents = open(fname, "rb").read()
                except EnvironmentError, e:
                    if not e.filename:
                        e.filename = fname
                    raise

            return contents

        def get_content_hash(self):
            return SCons.Util.MD5signature(self.get_contents())

    env['ABIDW'] = _detect(env)

    def ShlibNode(env, name, directory = None, create = 1):
        return env.fs._lookup(env.subst(name), directory, AbilinkNode, create)

    env.AddMethod(ShlibNode, 'ShlibNode')

    def shlib_target_factory(arg):
        return env.ShlibNode(arg)

    env['BUILDERS']['SharedLibrary'].target_factory = shlib_target_factory

def exists(env):
    result = _detect(env) != None
    return result
