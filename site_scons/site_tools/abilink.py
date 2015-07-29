# Copyright (C) 2015 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

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
            try:
                # TODO: If there were python bindings for libabigail, we
                # could avoid the shell out (and probably be faster, as we
                # could get exactly the information we want).
                contents = subprocess.check_output([env.subst('$ABIDW'), fname])
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
