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

import re
import subprocess

import SCons


def exists(env):
    if "AR" not in env:
        return False

    ar = env.subst(env["AR"])
    if not ar:
        return False

    # If the user has done anything confusing with ARFLAGS, bail out. We want to find
    # an item in ARFLAGS of the exact form 'rc'.
    if "rc" not in env["ARFLAGS"]:
        return False

    pipe = SCons.Action._subproc(
        env,
        SCons.Util.CLVar(ar) + ["--version"],
        stdin="devnull",
        stderr="devnull",
        stdout=subprocess.PIPE,
    )
    if pipe.wait() != 0:
        return False

    found = False
    for line in pipe.stdout:
        if found:
            continue  # consume all data
        found = re.search(r"^GNU ar|^LLVM", line.decode("utf-8"))

    return bool(found)


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

        # Ninja uses only timestamps for implicit dependencies so will
        # always rebuild a program whose archive has been updated even
        # if has the same content signature.
        if env.get("GENERATING_NINJA", False):
            return old_results

        new_results = []
        for base in old_results:
            new_results.append(base)
            if getattr(env.Entry(base).attributes, "thin_archive", None):
                new_results.extend(base.children())

        return new_results

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner,
        path_function=path_function,
    )


def generate(env):
    if not exists(env):
        return

    env["ARFLAGS"] = SCons.Util.CLVar(
        [arflag if arflag != "rc" else "rcsTD" for arflag in env["ARFLAGS"]]
    )

    # Disable running ranlib, since we added 's' above
    env["RANLIBCOM"] = ""
    env["RANLIBCOMSTR"] = "Skipping ranlib for thin archive $TARGET"

    for builder in ["StaticLibrary", "SharedArchive"]:
        _add_emitter(env["BUILDERS"][builder])

    for builder in ["SharedLibrary", "LoadableModule", "Program"]:
        _add_scanner(env["BUILDERS"][builder])
