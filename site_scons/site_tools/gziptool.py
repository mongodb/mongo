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
import gzip
import shutil

def GZipAction(target, source, env, **kw):
    dst_gzip = gzip.GzipFile(str(target[0]), 'wb')
    with open(str(source[0]), 'r') as src_file:
        shutil.copyfileobj(src_file, dst_gzip)
    dst_gzip.close()

def generate(env, **kwargs):
    env['BUILDERS']['__GZIPTOOL'] = SCons.Builder.Builder(
        action=SCons.Action.Action(GZipAction, "$GZIPTOOL_COMSTR")
    )
    env['GZIPTOOL_COMSTR'] = kwargs.get(
        "GZIPTOOL_COMSTR",
        "Compressing $TARGET with gzip"
    )

    def GZipTool(env, target, source):
        result = env.__GZIPTOOL(target=target, source=source)
        env.AlwaysBuild(result)
        return result

    env.AddMethod(GZipTool, 'GZip')

def exists(env):
    return True
