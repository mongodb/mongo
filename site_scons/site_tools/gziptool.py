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

import gzip
import shutil

import SCons


def GZipAction(target, source, env, **kw):
    dst_gzip = gzip.GzipFile(str(target[0]), "wb")
    with open(str(source[0]), "rb") as src_file:
        shutil.copyfileobj(src_file, dst_gzip)
    dst_gzip.close()


def generate(env, **kwargs):
    env["BUILDERS"]["__GZIPTOOL"] = SCons.Builder.Builder(
        action=SCons.Action.Action(
            GZipAction,
            "$GZIPTOOL_COMSTR",
        )
    )
    env["GZIPTOOL_COMSTR"] = kwargs.get(
        "GZIPTOOL_COMSTR",
        "Compressing $TARGET with gzip",
    )

    def GZipTool(env, target, source, **kwargs):
        result = env.__GZIPTOOL(target=target, source=source, **kwargs)
        env.AlwaysBuild(result)
        return result

    env.AddMethod(GZipTool, "GZip")


def exists(env):
    return True
