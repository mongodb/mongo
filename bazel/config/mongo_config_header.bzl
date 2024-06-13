load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

def mongo_config_header_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    compiler_bin = cc_toolchain.compiler_executable
    output = ctx.actions.declare_file(ctx.attr.output.files.to_list()[0].path)

    # Generate compiler flags we need to make clang/gcc/msvc actually compile.
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.cxxopts + ctx.fragments.cpp.copts,
    )
    compiler_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = compile_variables,
    )

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    generator_script = ctx.attr.generator_script.files.to_list()[0].path

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [output],
        inputs = depset(transitive = [cc_toolchain.all_files, python.files, ctx.attr.generator_script.files]),
        arguments = [
            generator_script,  # bazel/config/generate_config_header.py
            "--compiler-path",
            compiler_bin,
            "--compiler-args",
            " ".join(compiler_flags),
            "--output-path",
            output.path,
        ],
    )

    return [DefaultInfo(files = depset([output]))]

mongo_config_header = rule(
    mongo_config_header_impl,
    attrs = {
        "output": attr.label(
            doc = "The output of this rule.",
            allow_single_file = True,
        ),
        "generator_script": attr.label(
            doc = "The python generator script to use.",
            default = "//bazel/config:generate_config_header",
        ),
        "_cc_toolchain": attr.label(default = "@bazel_tools//tools/cpp:current_cc_toolchain"),
    },
    fragments = ["cpp"],
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type", "@bazel_tools//tools/python:toolchain_type"],
    output_to_genfiles = True,
)
