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


def _tag_as_precious(target, source, env):
    env.Precious(target)
    return target, source


def generate(env):
    builders = env["BUILDERS"]
    for builder in ("Program", "SharedLibrary", "LoadableModule"):
        emitter = builders[builder].emitter
        builders[builder].emitter = SCons.Builder.ListEmitter(
            [emitter, _tag_as_precious,]
        )


def exists(env):
    # By default, the windows linker is incremental, so unless
    # overridden in the environment with /INCREMENTAL:NO, the tool is
    # in play.
    if env.TargetOSIs("windows") and not "/INCREMENTAL:NO" in env["LINKFLAGS"]:
        return True

    # On posix platforms, excluding darwin, we may have enabled
    # incremental linking. Check for the relevant flags.
    if (
        env.TargetOSIs("posix")
        and not env.TargetOSIs("darwin")
        and "-fuse-ld=gold" in env["LINKFLAGS"]
        and "-Wl,--incremental" in env["LINKFLAGS"]
    ):
        return True

    return False
