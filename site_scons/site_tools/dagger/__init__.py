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

"""The initialization for the dagger tool. This file provides the initialization for the tool
and attaches our custom builders and emitters to the build process"""
import os
import logging

import SCons

from . import dagger


def generate(env, **kwargs):
    """The entry point for our tool. However, the builder for
    the JSON file is not actually run until the Dagger method is called
    in the environment. When we generate the tool we attach our emitters
    to the native builders for object/libraries.
    """

    env.Replace(
        LIBEMITTER=SCons.Builder.ListEmitter(
            [env["LIBEMITTER"], dagger.emit_lib_db_entry]
        )
    )
    running_os = os.sys.platform

    if not (running_os.startswith("win") or running_os.startswith("sun")):
        env.Replace(
            PROGEMITTER=SCons.Builder.ListEmitter(
                [env["PROGEMITTER"], dagger.emit_prog_db_entry]
            )
        )

    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)
    suffixes = [".c", ".cc", ".cxx", ".cpp"]
    obj_builders = [static_obj, shared_obj]
    default_emitters = [
        SCons.Defaults.StaticObjectEmitter,
        SCons.Defaults.SharedObjectEmitter,
    ]

    for suffix in suffixes:
        for i in range(len(obj_builders)):
            obj_builders[i].add_emitter(
                suffix,
                SCons.Builder.ListEmitter(
                    [dagger.emit_obj_db_entry, default_emitters[i]]
                ),
            )

    env["BUILDERS"]["__OBJ_DATABASE"] = SCons.Builder.Builder(
        action=SCons.Action.Action(dagger.write_obj_db, None)
    )

    def Dagger(env, target="library_dependency_graph.json"):
        if running_os.startswith("win") or running_os.startswith("sun"):
            logging.error("Dagger is only supported on OSX and Linux")
            return
        result = env.__OBJ_DATABASE(target=target, source=[])
        env.AlwaysBuild(result)
        env.NoCache(result)

        return result

    env.AddMethod(Dagger, "Dagger")


def exists(env):
    return True
