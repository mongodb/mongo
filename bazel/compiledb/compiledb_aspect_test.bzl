load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load("//bazel:mongo_src_rules.bzl", "mongo_cc_fuzztest")
load(":compiledb_aspect.bzl", "CompileCommandInfo", "compiledb_aspect", "is_msvc_compiler")

def _is_msvc_compiler_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.true(env, is_msvc_compiler("C:/Program Files/LLVM/bin/clang-cl.exe"))
    asserts.true(env, is_msvc_compiler("external/toolchain/tools/clang-cl"))
    asserts.true(env, is_msvc_compiler("external/toolchain/wrappers/clang-cl"))
    asserts.true(env, is_msvc_compiler("C:/Program Files/Microsoft Visual Studio/cl.exe"))
    asserts.true(env, is_msvc_compiler("C:\\Program Files\\Microsoft Visual Studio\\cl"))

    asserts.false(env, is_msvc_compiler("external/toolchain/bin/clang++"))
    asserts.false(env, is_msvc_compiler("external/toolchain/bin/gcc"))

    return unittest.end(env)

is_msvc_compiler_test = unittest.make(_is_msvc_compiler_test_impl)

def _fuzztest_compile_commands_test_impl(ctx):
    env = analysistest.begin(ctx)
    target = analysistest.target_under_test(env)
    fragments = target[CompileCommandInfo].files.to_list()
    asserts.true(
        env,
        any([fragment.basename.startswith(ctx.attr.source + ".") for fragment in fragments]),
        "Missing compile command fragment for " + ctx.attr.source,
    )
    return analysistest.end(env)

_fuzztest_compile_commands_test = analysistest.make(
    _fuzztest_compile_commands_test_impl,
    config_settings = {
        str(Label("//bazel/config:compiledb")): True,
        str(Label("//bazel/config:fsan")): False,
        str(Label("@fuzztest//fuzztest:use_riegeli")): False,
    },
    extra_target_under_test_aspects = [compiledb_aspect],
    attrs = {"source": attr.string(mandatory = True)},
)

def fuzztest_compile_commands_test_suite(name):
    mongo_cc_fuzztest(
        name = "compiledb_fuzztest_fixture",
        srcs = ["testdata/compiledb_fuzztest.cpp"],
        tags = ["manual"],
        skip_global_deps = ["allocator", "libunwind"],
    )
    _fuzztest_compile_commands_test(
        name = name + "_fuzztest",
        target_under_test = ":compiledb_fuzztest_fixture_with_debug",
        source = "compiledb_fuzztest.cpp",
        size = "small",
    )
    _fuzztest_compile_commands_test(
        name = name + "_bson_mutator",
        target_under_test = "//src/mongo/bson/bson_mutator",
        source = "bson_mutator.cpp",
        size = "small",
    )
    _fuzztest_compile_commands_test(
        name = name + "_deprecated_fuzzer",
        target_under_test = "//src/mongo/bson:bson_validate_fuzzer_with_debug",
        source = "bson_validate_fuzzer.cpp",
        size = "small",
    )
    native.test_suite(
        name = name,
        tests = [
            ":" + name + "_fuzztest",
            ":" + name + "_bson_mutator",
            ":" + name + "_deprecated_fuzzer",
        ],
    )
