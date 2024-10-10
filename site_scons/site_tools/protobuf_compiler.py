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

import contextlib
import os
import subprocess
import tempfile

import SCons


# context manager copied from
# https://stackoverflow.com/a/57701186/1644736
@contextlib.contextmanager
def temporary_filename(suffix=None):
    """Context that introduces a temporary file.

    Creates a temporary file, yields its name, and upon context exit, deletes it.
    (In contrast, tempfile.NamedTemporaryFile() provides a 'file' object and
    deletes the file as soon as that file object is closed, so the temporary file
    cannot be safely re-opened by another library or process.)

    Args:
      suffix: desired filename extension (e.g. '.mp4').

    Yields:
      The name of the temporary file.
    """
    try:
        f = tempfile.NamedTemporaryFile(suffix=suffix, delete=False)
        tmp_name = f.name
        f.close()
        yield tmp_name
    finally:
        os.unlink(tmp_name)


def get_gen_type_and_dir(env, gen_type):
    # Utility function for parsing out the gen type and desired gen dir
    if SCons.Util.is_String(gen_type):
        gen_out_dir = None
    elif SCons.Util.is_List(gen_type) and len(gen_type) == 1:
        gen_type = gen_type[0]
        gen_out_dir = None
    elif SCons.Util.is_List(gen_type) and len(gen_type) == 2:
        gen_out_dir = gen_type[1]
        gen_type = gen_type[0]
    else:
        raise ValueError(
            f"Invalid generation type {gen_type}, must be string of gen type, or list of gen type and gen out dir."
        )
    return (gen_type, gen_out_dir)


def protoc_emitter(target, source, env):
    new_targets = []
    gen_types = env.subst_list("$PROTOC_GEN_TYPES", target=target, source=source)
    base_file_name = os.path.splitext(target[0].get_path())[0]
    for gen_type in gen_types:
        # Check for valid requested gen type.
        gen_type, gen_out_dir = get_gen_type_and_dir(env, gen_type)

        if gen_type not in env["_PROTOC_SUPPORTED_GEN_TYPES"]:
            raise ValueError(
                f"Requested protoc gen output of {gen_type}, but only {env['_PROTOC_SUPPORTED_GEN_TYPES']} are currenlty supported."
            )

        if gen_out_dir:
            base_file_name = os.path.join(
                env.Dir(gen_out_dir).get_path(),
                os.path.split(SCons.Util.splitext(target[0].get_path())[0])[1],
            )

        # Create the targets by extensions list for this type in the desired gen dir.
        exts = env["_PROTOC_SUPPORTED_GEN_TYPES"][gen_type]
        new_targets += [env.File(f"{base_file_name}{ext}") for ext in exts]

    if gen_types:
        # Setup the dependency file.
        # This is little weird currently, because of the limitation of ninja and multiple
        # outputs. The base file name can change for each gen type, so in this case we are
        # taking the last one. This works if all gen outs are in the same dir and makes ninja
        # happy, but if there are multiple gen_out dirs, then in a scons only build the deps
        # is gened to the last in the list, which is awkward, but because this is only refernced
        # as a target throughout the rest of tool, it works fine in scons build.
        dep_file = env.File(f"{base_file_name}.protodeps")
        new_targets += [dep_file]

    # Create targets for any listed plugins.
    plugins = env.get("PROTOC_PLUGINS", [])
    for name in plugins:
        out_dir = plugins[name].get("gen_out")
        exts = plugins[name].get("exts", [])

        if out_dir:
            base_file_name = os.path.join(
                env.Dir(out_dir).get_path(),
                os.path.split(SCons.Util.splitext(target[0].get_path())[0])[1],
            )

        new_targets += [env.File(f"{base_file_name}{ext}") for ext in exts]

    return new_targets, source


def protoc_scanner(node, env, path):
    deps = []

    # Need to depend on the compiler and any plugins.
    plugins = env.get("PROTOC_PLUGINS", {})
    for name in plugins:
        deps.append(env.File(env.subst(plugins[name].get("plugin"))))
    deps.append(env.File("$PROTOC"))

    # For scanning the proto dependencies from within the proto files themselves,
    # there are two ways (with out writing a custom reader) to do it. One is with the
    # output depends file and other other is with a tool the protobuf project supplies.
    # The problem with the depends files, is you must first run the command before you can
    # get the dependencies, which has some downsides:
    # https://scons.org/doc/4.4.0/HTML/scons-user.html#idp105548894482512
    #
    # Using the reader provided by protobuf project works, but you must have access to the
    # proto which gives this functionality.
    #
    # Scanners will run multiple times during the building phase, revisiting as new dependencies
    # from the original scan are completed. Here we will use both methods, because in the case
    # you have an existing dep file you can get more dependency information on the first scan.
    if str(node).endswith(".protodeps"):
        if os.path.exists(node.get_path()):
            # This code was mostly ripped from SCons ParseDepends function
            try:
                with open(node.get_path(), "r") as fp:
                    lines = SCons.Util.LogicalLines(fp).readlines()
            except IOError:
                pass
            else:
                lines = [l for l in lines if l[0] != "#"]
                for line in lines:
                    try:
                        target, depends = line.split(":", 1)
                    except (AttributeError, ValueError):
                        # Throws AttributeError if line isn't a string.  Can throw
                        # ValueError if line doesn't split into two or more elements.
                        pass
                    else:
                        deps += [env.File(d) for d in depends.split()]

        if os.path.exists(env.File("$PROTOC").abspath) and os.path.exists(
            env.File("$PROTOC_DESCRIPTOR_PROTO").abspath
        ):
            # First we generate a the command line so we can extract the proto_paths as they
            # used for finding imported protos. Then we run the command and output the
            # descriptor set to a file for use later. The descriptor set is output as binary data
            # intended to be read in by other protos. In this case the second command does that
            # and extracts the dependencies.
            source = node.sources[0]
            with temporary_filename() as temp_filename:
                cmd_list, _, _ = env["BUILDERS"]["Protoc"].action.process(
                    [node], [source], env, executor=None
                )

                paths = [
                    str(proto_path)
                    for proto_path in cmd_list[0]
                    if str(proto_path).startswith("--proto_path=")
                ]
                cmd = (
                    [env.File("$PROTOC").path]
                    + paths
                    + [
                        "--include_imports",
                        f"--descriptor_set_out={temp_filename}",
                        source.srcnode().path,
                    ]
                )

                subprocess.run(cmd)
                with open(temp_filename) as f:
                    cmd = (
                        [env.File("$PROTOC").path]
                        + paths
                        + [
                            "--decode=google.protobuf.FileDescriptorSet",
                            str(env.File("$PROTOC_DESCRIPTOR_PROTO")),
                        ]
                    )

                    p = subprocess.run(cmd, stdin=f, capture_output=True)
                    for line in p.stdout.decode().splitlines():
                        if line.startswith('  name: "'):
                            file = line[len('  name: "') : -1]
                            for path in paths:
                                proto_file = os.path.join(path.replace("--proto_path=", ""), file)
                                if os.path.exists(proto_file) and proto_file != str(
                                    source.srcnode()
                                ):
                                    dep_node = env.File(proto_file)
                                    if dep_node not in deps:
                                        deps += [env.File(proto_file)]
                                    break

    return sorted(deps, key=lambda dep: dep.path)


protoc_scanner = SCons.Scanner.Scanner(function=protoc_scanner)


def get_cmd_line_dirs(env, target, source):
    source_dir = os.path.dirname(source[0].srcnode().path)
    target_dir = os.path.dirname(target[0].get_path())

    return target_dir, source_dir


def gen_types(source, target, env, for_signature):
    # This subst function is for generating the command line --proto_path and desired
    # --TYPE_out options.
    cmd_flags = ""
    gen_types = env.subst_list("$PROTOC_GEN_TYPES", target=target, source=source)
    if gen_types:
        for gen_type in gen_types:
            gen_type, gen_out_dir = get_gen_type_and_dir(env, gen_type)
            exts = tuple(env["_PROTOC_SUPPORTED_GEN_TYPES"][gen_type])

            gen_targets = [t for t in target if str(t).endswith(exts)]
            if gen_targets:
                out_dir, proto_path = get_cmd_line_dirs(env, gen_targets, source)
                cmd_flags += f" --proto_path={proto_path} --{gen_type}_out={out_dir}"

        # This depends out only works if there is at least one gen out
        for t in target:
            if str(t).endswith(".protodeps"):
                cmd_flags = f"--dependency_out={t} " + cmd_flags

    return cmd_flags


def gen_types_str(source, target, env, for_signature):
    # This generates the types from the list of types requested by the user
    # for the pretty build output message. Any invalid types are caught in the emitter.
    gen_types = []
    for gen_type in env.subst_list("$PROTOC_GEN_TYPES", target=target, source=source):
        gen_type, gen_out_dir = get_gen_type_and_dir(env, gen_type)
        gen_types += [str(gen_type)]

    return ", ".join(gen_types)


def gen_plugins(source, target, env, for_signature):
    # Plugins are user customizable ways to modify the generation and generate
    # additional files if desired. This extracts the desired plugins from the environment
    # and formats them to be suitable for the command line.
    plugins_cmds = []
    plugins = env.get("PROTOC_PLUGINS", [])

    for name in plugins:
        plugin = plugins[name].get("plugin")
        exts = plugins[name].get("exts")
        if plugin and exts:
            out_dir = plugins[name].get("gen_out", ".")
            options = plugins[name].get("options", [])

            # A custom out command for this plugin, options to the plugin can
            # be passed here with colon separating
            cmd_line = f"--{name}_out="
            for opt in options:
                cmd_line += f"{opt}:"

            gen_targets = [t for t in target if str(t).endswith(tuple(exts))]
            if gen_targets:
                out_dir, proto_path = get_cmd_line_dirs(env, gen_targets, source)
                cmd_line += out_dir

                # specify the plugin binary
                cmd_line += (
                    f" --proto_path={proto_path} --plugin=protoc-gen-{name}={env.File(plugin).path}"
                )
                plugins_cmds += [cmd_line]
        else:
            print(
                f"Failed to process PROTOC plugin, need valid plugin and extensions {name}: {plugins[name]}"
            )

    gen_types = env.subst_list("$PROTOC_GEN_TYPES", target=target, source=source)
    # In the case the command did not include any standard gen types, we add a command line
    # entry so the depends file is still written
    if not gen_types:
        for t in target:
            if str(t).endswith(".protodeps"):
                plugins_cmds += [f"--dependency_out={t}"]

    return " ".join(plugins_cmds)


def generate(env):
    ProtocBuilder = SCons.Builder.Builder(
        action=SCons.Action.Action("$PROTOCCOM", "$PROTOCCOMSTR"),
        emitter=protoc_emitter,
        src_suffix=".proto",
        suffix=".cc",
        target_scanner=protoc_scanner,
    )

    env.Append(SCANNERS=protoc_scanner)
    env["BUILDERS"]["Protoc"] = ProtocBuilder

    env["PROTOC"] = env.get("PROTOC", env.WhereIs("protoc"))
    env["PROTOCCOM"] = (
        "$PROTOC $_PROTOCPATHS $_PROTOC_GEN_TYPES $_PROTOC_PLUGINS $PROTOCFLAGS $SOURCE"
    )
    env["PROTOCCOMSTR"] = (
        "Generating $_PROTOC_GEN_TYPES_STR Protocol Buffers from ${SOURCE}"
        if not env.Verbose()
        else ""
    )

    # Internal subst function vars
    env["_PROTOC_GEN_TYPES"] = gen_types
    env["_PROTOC_GEN_TYPES_STR"] = gen_types_str
    env["_PROTOC_PLUGINS"] = gen_plugins
    env["_PROTOCPATHS"] = "${_concat(PROTOPATH_PREFIX, PROTOCPATHS, PROTOPATH_SUFFIX, __env__)}"

    env["PROTOPATH_PREFIX"] = "--proto_path="
    env["PROTOPATH_SUFFIX"] = ""

    # Somewhat safe cross tool dependency
    if hasattr(env, "NinjaGenResponseFileProvider"):
        env.NinjaRule(
            rule="PROTO",
            command="$env$cmd",
            description="Generating protocol buffers $out",
            deps="gcc",
            use_depfile=True,
            depfile="$protodep",
        )

        def gen_protobuf_provider(env, rule, tool):
            def protobuf_provider(env, node, action, targets, sources, executor=None):
                provided_rule, variables, tool_command = env.NinjaGetGenericShellCommand(
                    node, action, targets, sources, executor
                )

                t_dirs = [os.path.dirname(t.get_path()) for t in targets]
                if len(set(t_dirs)) > 1:
                    raise SCons.Errors.BuildError(
                        node=node,
                        errstr="Due to limitations with ninja tool and using phonies for multiple targets, protoc must generate all generated output for a single command to the same directory.",
                    )
                for t in targets:
                    if str(t).endswith(".protodeps"):
                        variables["protodep"] = str(t)
                return "PROTO", variables, tool_command

            return protobuf_provider

        def robust_rule_mapping(var, rule, tool):
            provider = gen_protobuf_provider(env, rule, tool)
            env.NinjaRuleMapping("${" + var + "}", provider)
            env.NinjaRuleMapping(env[var], provider)

        robust_rule_mapping("PROTOCCOM", "PROTO", "$PROTOC")

    # TODO create variables to support other generation types, might require a more flexible
    # builder setup
    env["_PROTOC_SUPPORTED_GEN_TYPES"] = {"cpp": [".pb.cc", ".pb.h"]}

    # User facing customizable variables

    # PROTOC_GEN_TYPES can be a list of strings, where
    # each string is the gen type desired, or it could
    # a list of lists, where each list contains first
    # the type, the the desired output dir, if no
    # dir is specified the scons will build it at the location
    # of the source proto file, accounting for variant
    # dirs. e.g.
    # env["PROTOC_GEN_TYPES"] = [
    #     'cpp',
    #     ['java', "$BUILD_DIR/java_gen_source"]
    # ]
    env["PROTOC_GEN_TYPES"] = []

    # PROTOC_PLUGINS allows customization of the plugins
    # for the command lines. It should be a dict of dicts where
    # the keys are the names of the plugins, and the plugin must
    # specify the plugin binary file path and a list of extensions
    # to use on the output files. Optionally you can specify a list
    # of options to pass the plugin and a gen out directory. e.g:
    # env['PROTOC_PLUGINS']={
    #     'grpc': {
    #         'plugin': '$PROTOC_GRPC_PLUGIN',
    #         'options': ['generate_mock_code=true'],
    #         'gen_out': "$BUILD_DIR/grpc_gen"
    #         'exts': ['.grpc.pb.cc', '.grpc.pb.h'],
    #     },
    #     'my_plugin': {
    #         'plugin': '/usr/bin/my_custom_plugin',
    #         'exts': ['.pb.txt'],
    #     }
    # },
    env["PROTOC_PLUGINS"] = {}

    # This is a proto which allows dependent protos to be extracted
    # generally this is in protobuf src tree at google/protobuf/descriptor.proto
    env["PROTOC_DESCRIPTOR_PROTO"] = "google/protobuf/descriptor.proto"

    env["PROTOCFLAGS"] = SCons.Util.CLVar("")
    env["PROTOCPATHS"] = SCons.Util.CLVar("")


def exists(env):
    return True
