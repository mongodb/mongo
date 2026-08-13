load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":compiledb_aspect.bzl", "is_msvc_compiler")

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
