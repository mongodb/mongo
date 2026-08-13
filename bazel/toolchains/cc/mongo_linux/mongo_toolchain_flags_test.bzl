load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":mongo_toolchain_flags_v5.bzl", "clang_resource_dir")

def _clang_resource_dir_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "external/mongo_toolchain_v5/stow/llvm-v5/lib/clang/19/",
        clang_resource_dir("mongo_toolchain_v5"),
    )
    return unittest.end(env)

clang_resource_dir_test = unittest.make(_clang_resource_dir_test_impl)

def mongo_toolchain_flags_test_suite(name):
    unittest.suite(
        name,
        clang_resource_dir_test,
    )
