"""Analyze the optional Python-wheel execution group under a Windows target."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//bazel/config:configs.bzl", "compiler_type_provider")

_CompilerInfo = provider(fields = ["compiler", "tool_compiler"])

def _probe_impl(ctx):
    return [_CompilerInfo(
        compiler = ctx.attr._compiler[compiler_type_provider].compiler_type,
        tool_compiler = ctx.attr.tool[_CompilerInfo].compiler if ctx.attr.tool else None,
    )]

_probe = rule(
    implementation = _probe_impl,
    attrs = {
        "_compiler": attr.label(default = "//bazel/config:compiler_type"),
        "tool": attr.label(cfg = config.exec(exec_group = "cross")),
    },
    exec_groups = {
        "cross": exec_group(exec_compatible_with = ["//bazel/platforms:use_mongo_linux_cross_execution"]),
    },
)

def _test_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)[_CompilerInfo]
    asserts.equals(env, "msvc", target.compiler)
    asserts.equals(env, "clang", target.tool_compiler)
    return analysistest.end(env)

_test = analysistest.make(
    _test_impl,
    config_settings = {
        "//command_line_option:platforms": "@@//bazel/platforms:windows_x86_64_cross",
        "@@//bazel/config:compiler_type": "msvc",
    },
)

def windows_cross_exec_test(name):
    _probe(name = name + "_tool", tags = ["manual"])
    _probe(name = name + "_probe", tool = ":" + name + "_tool", tags = ["manual"])
    _test(name = name, target_under_test = ":" + name + "_probe")
