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
import subprocess

# TODO: DRY this with abilink.py by moving duplicated code out to a common
# support module.

def _detect(env):
    try:
        tapi = env["TAPI"]
        if not tapi:
            return None
        return tapi
    except KeyError:
        pass

    return env.WhereIs("tapi")


def _add_emitter(builder):
    base_emitter = builder.emitter

    def new_emitter(target, source, env):
        new_targets = []
        for t in target:
            base, ext = SCons.Util.splitext(str(t))
            if not ext == env['SHLIBSUFFIX']:
                continue

            tbd_target = (t.builder.target_factory or env.File)(base + ".tbd")
            new_targets.append(tbd_target)

            tbd_no_uuid_target = (t.builder.target_factory or env.File)(base + ".tbd.no_uuid")
            new_targets.append(tbd_no_uuid_target)

            setattr(t.attributes, "tbd", tbd_no_uuid_target)
        targets = target + new_targets
        return (targets, source)

    new_emitter = SCons.Builder.ListEmitter([base_emitter, new_emitter])
    builder.emitter = new_emitter


def _add_scanner(builder):
    old_scanner = builder.target_scanner
    path_function = old_scanner.path_function

    def new_scanner(node, env, path):
        return (getattr(env.Entry(o).attributes, "tbd", o) for o in old_scanner(node, env, path))

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner, path_function=path_function
    )

def _add_action(builder):
    actions = builder.action

    # The first inbocation of TAPI is to make the tbd file that the
    # linker will actually use when linking. This must contain the
    # dylib UUID or the link will fail. The second creates a version
    # that does not contain the UUID. We use that as the ABI file. If
    # invoking TAPI proves to be expensive, we could address this by
    # instead post-processing the "real" .tbd file to strip out the
    # UUID, and then potentially even feed it into a hash algorithm.
    builder.action = actions + SCons.Action.Action(
        [
            "$TAPI stubify -o ${TARGET.base}.tbd ${TARGET}",
            "$TAPI stubify --no-uuids -o ${TARGET.base}.tbd.no_uuid ${TARGET}"
        ]
    )

def exists(env):
    result = _detect(env) != None
    return result


def generate(env):

    if not exists(env):
        return

    builder = env["BUILDERS"]["SharedLibrary"]
    _add_emitter(builder)
    _add_action(builder)
    _add_scanner(builder)
    _add_scanner(env["BUILDERS"]["Program"])
    _add_scanner(env["BUILDERS"]["LoadableModule"])
