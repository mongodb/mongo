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
import traceback

import SCons

def protoc_emitter(target, source, env):

    first_source = str(source[0])

    if not first_source.endswith(".proto"):
        raise ValueError("Bad protoc file name '%s', it must end with '.proto' " % (first_source))

    base_file_name, _ = SCons.Util.splitext(str(target[0]))
    new_targets = []
    for type in env.get('PROTOC_GEN_TYPES', []):
        if type == 'cpp':
            new_targets += [f"{base_file_name}.pb.cc", f"{base_file_name}.pb.h"]

    plugins = env.get('PROTOC_PLUGINS', [])
    for name in plugins:
        for ext in plugins[name].get('exts', []):
            new_targets += [f"{base_file_name}{ext}"]

    env.Alias("generated-sources", new_targets)

    return new_targets, source

def protoc_scanner(node, env, path):

    # TODO Make this scanner understand how to parse .proto files to find .proto
    # dependencies.
    deps = []
    plugins = env.get('PROTOC_PLUGINS', {})
    for name in plugins:
        deps.append(env.File(env.subst(plugins[name].get('plugin'))))

    deps.append(env.File("$PROTOC"))

    return deps

protoc_scanner = SCons.Scanner.Scanner(function=protoc_scanner, skeys=[".proto"])

def type_out_gen(source, target, env, for_signature):
    # the user set PROTOC_CPPGEN_DIR that means we add the flag
    # to turn on cpp generation
    try:
        result = env.subst('$PROTOC_GEN_TYPES', target=target, source=source)
        if 'cpp' in result:
            out_dir = os.path.dirname(str(target[0].abspath))
            os.makedirs(out_dir, exist_ok=True)
            return f"--cpp_out={out_dir}"
    except:
        traceback.print_exc()

def gen_types_str(source, target, env, for_signature):
    # This generates the types from the list of types we which are supported and
    # which the user has set an output dir for. This is for build output messages.
    gen_types_results = []
    for gen_dir, type in env.get('_PROTOC_SUPPORTED_GEN_TYPES', []):
        result = env.subst(gen_dir, target=target, source=source)
        if result:
            gen_types_results += [type]
    return ','.join(gen_types_results)

def gen_types(source, target, env, for_signature):
    # This generates the types from the list of types we which are supported and
    # which the user has set an output dir for. This selects the types to add to
    # the command line to then be expanded to --XXX_out options later.
    gen_types_results = []
    for gen_dir, type in env.get('_PROTOC_SUPPORTED_GEN_TYPES', []):
        result = env.subst(gen_dir, target=target, source=source)
        if result:
            gen_types_results += [result]
    return ' '.join(gen_types_results)

def gen_plugins(source, target, env, for_signature):
    # Plugins are user customizable ways to modify the generation and generate
    # additional files if desired. This extracts the desired plugins from the environment
    # and formats them to be suitable for the command line.
    gen_types_results = []
    plugins = env.get('PROTOC_PLUGINS', [])
    for name in plugins:
        out_dir = plugins[name].get('out_dir')
        plugin = plugins[name].get('plugin')
        options = plugins[name].get('options')
        if plugin and out_dir:
            cmd_line = f'--{name}_out='
            for opt in options:
                cmd_line += f'{opt}:'
            out_dir = os.path.dirname(str(target[0].abspath))
            os.makedirs(out_dir, exist_ok=True)
            cmd_line += out_dir
            cmd_line += f' --plugin=protoc-gen-{name}={env.File(plugin).abspath}'
            gen_types_results += [cmd_line]
        else:
            print(f"Failed to process PROTOC plugin {name}: {plugins[name]}")

    return ' '.join(gen_types_results)


def protocActionGen(source, target, env, for_signature):
    # The protobuf compiler doesn't play nice with scons variant dir. It will output its generated
    # files in the specified out dir + the source path given to it. For scons variant dir where we
    # want to put things from "src" into "BUILD_DIR" we would have to somehow pass it a source file
    # without the "src" prefixing it. This means we must change working directory where scons
    # normally executes in the src root directory. Because we manage the pathing flags within
    # the scons variables, we can accurately update any pathing to allow us to switch to source
    # location and perform the command there.

    working_dir = os.path.dirname(target[0].srcnode().path)
    protoc_path = os.path.abspath(env.subst('$PROTOC'))

    return env.subst(f"cd {working_dir} && {protoc_path} $_PROTOC_GEN_TYPES $_PROTOC_PLUGINS  $PROTOCFLAGS ${{SOURCE.file}}", target=target, source=source)

def generate(env):

    ProtocAction = SCons.Action.CommandGeneratorAction(
        protocActionGen,
        {"cmdstr": "Generating $_PROTOC_GEN_TYPES_STR Protocol Buffers from ${SOURCE}"}
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

    # These vars setup the different generation options
    env["_PROTOC_GEN_TYPES"] = gen_types
    env['_PROTOC_GEN_TYPES_STR'] = gen_types_str
    env['_PROTOC_PLUGINS'] = gen_plugins

    # The general way to default the out dir for any given type
    env['_PROTOC_DEFAULT_GENOUT'] = type_out_gen

    # TODO create variables to support other generation types, might require a more flexible
    # builder setup
    env["_PROTOC_SUPPORTED_GEN_TYPES"] = [('$_PROTOC_DEFAULT_GENOUT', 'cpp')]

    # User facing customizable variables
    env["PROTOC_GEN_TYPES"] = []
    env['PROTOC_PLUGINS'] = {}

    env['PROTOCFLAGS'] = SCons.Util.CLVar('')

def exists(env):
    return True
