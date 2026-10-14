"""Ensure every cross compiler/runtime pair stays on its own RHEL release."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":mongo_linux_cross_toolchain.bzl", "cross_selector", "gold_sysroot_script")
load(":mongo_toolchain_version_v4.bzl", "S390X_GCC_TOOLCHAINS")
load(":sysroot_containers.bzl", "S390X_SYSROOT_CONTAINERS")

def _matrix_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, ["rhel8", "rhel9", "rhel10"], S390X_GCC_TOOLCHAINS.keys())
    asserts.equals(env, S390X_GCC_TOOLCHAINS.keys(), S390X_SYSROOT_CONTAINERS.keys())
    for distro, info in S390X_GCC_TOOLCHAINS.items():
        asserts.equals(env, distro + "_s390x_on_" + distro + "_aarch64", cross_selector(distro))
        asserts.equals(env, 64, len(info["sha"]))
        asserts.true(env, "bazel_v4_toolchain-" in info["url"])
        asserts.true(env, "arm64" in info["url"])
        asserts.true(env, info["url"].startswith("https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/"))
        asserts.false(env, "patch" in info["url"])
        asserts.true(env, "@sha256:" in S390X_SYSROOT_CONTAINERS[distro])
    return unittest.end(env)

matrix_test = unittest.make(_matrix_test_impl)

def _gold_sysroot_script_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(
        env,
        "OUTPUT_FORMAT(elf64-s390)\nGROUP ( libgcc_s.so.1 libgcc.a )\n",
        gold_sysroot_script("OUTPUT_FORMAT(elf64-s390)\nGROUP ( =/usr/lib64/libgcc_s.so.1 libgcc.a )\n"),
    )
    asserts.equals(env, "INPUT ( libstdc++.a )\n", gold_sysroot_script("INPUT ( libstdc++.a )\n"))
    return unittest.end(env)

gold_sysroot_script_test = unittest.make(_gold_sysroot_script_test_impl)
