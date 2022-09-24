# Copyright 2022 MongoDB Inc.
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
"""Protobuf Compiler Scons Tool."""

import os

import SCons

def protoc_emitter(target, source, env):

    first_source = str(source[0])

    if not first_source.endswith(".proto"):
        raise ValueError("Bad protoc file name '%s', it must end with '.proto' " % (first_source))

    base_file_name, _ = SCons.Util.splitext(str(target[0]))

    message_source = env.File(base_file_name + ".grpc.pb.cc")
    message_header = env.File(base_file_name + ".grpc.pb.h")
    class_source = env.File(base_file_name + ".pb.cc")
    class_header = env.File(base_file_name + ".pb.h")
    new_targets = [message_source, message_header, class_source, class_header]

    # So we can put these in generated source alias because ninja must generate all sources before
    # it will compile anything because it doesn't know how to parse dependencies from generic
    # generation inputs. We could teach it by telling it a script to call during the build that
    # will output the dep file similar to how gcc will do for make with -MD. But we would need to
    # teach it for all generated source before we could stop gen_all before compile. For now we can
    # at least make compile_commands.json dependent on these so it still can be used for IDE work.
    #env.Alias("generated-sources", new_targets)
    env.Depends(env.File("#compile_commands.json"), new_targets)

    return new_targets, source

def protocActionGen(source, target, env, for_signature):
    working_dir = os.path.dirname(target[0].srcnode().path)
    cpp_out = os.path.dirname(target[0].path)
    os.makedirs(cpp_out, exist_ok=True)
    protoc_path = os.path.abspath(env.subst('$PROTOC'))
    env['PROTOC_GENOUT_DIR'] = os.path.abspath(cpp_out)

    # protoc doesn't play nice with scons variant dir. It will build in the --cpp_out dir,
    # but it always uses the path of the source inside the cpp_out dir. This means you end
    # up with really long paths, where most of its duplicated like:
    # $BUILD_DIR_PATH/$SOURCE_DIR_PATH/$GENERATED_SOURCE_FILE
    # The other option is to change to the source directory and pass the source directly.
    return env.subst(f"cd {working_dir} && {protoc_path} $PROTOCFLAGS ${{SOURCE.file}}", target=target, source=source)

def protoc_scanner(node, env, path):

    # TODO Make this scanner understand how to parse .proto files to find .proto
    # dependencies.
    deps = []
    protoc_flags = env.Flatten(env.subst_list("$PROTOCFLAGS"))
    for flag in protoc_flags:
        if '--plugin' in flag:
            # this option should have an '=', so we can always take the last token to
            # find the path
            #https://developers.google.com/protocol-buffers/docs/reference/cpp/google.protobuf.compiler.plugin
            deps.append(env.File(flag.split('=')[-1]))
    deps.append(env.File("$PROTOC"))

    return deps

protoc_scanner = SCons.Scanner.Scanner(function=protoc_scanner, skeys=[".proto"])

def generate(env):

    ProtocAction = SCons.Action.CommandGeneratorAction(
        protocActionGen,
        {"cmdstr": "Generating Protocal Buffers from ${SOURCE}"}
        if not env.get("VERBOSE", "").lower() in ['true', '1'] else {"cmdstr": ""},
    )

    ProtocBuilder = SCons.Builder.Builder(
        action=ProtocAction,
        emitter=protoc_emitter,
        src_suffix=".proto",
        suffix=".cc",
        source_scanner=protoc_scanner,
        single_source=1,
    )

    env.Append(SCANNERS=protoc_scanner)

    env["BUILDERS"]["Protoc"] = ProtocBuilder
    env["PROTOC"] = env.get('PROTOC', env.WhereIs('protoc'))
    env["PROTOC_GENOUT_DIR"] = "."
    env['PROTOCFLAGS'] = SCons.Util.CLVar('')

def exists(env):
    return True
