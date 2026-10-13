"""Ensure every cross compiler/runtime pair stays on its own RHEL release."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":mongo_linux_cross_toolchain.bzl", "cross_selector")
load(":mongo_toolchain_flags_v5.bzl", "COMMON_BINDIRS")
load(":mongo_toolchain_version_v5.bzl", "PPC64LE_GCC_TOOLCHAINS", "S390X_GCC_TOOLCHAINS")
load(":sysroot_containers.bzl", "PPC64LE_SYSROOT_CONTAINERS", "S390X_SYSROOT_CONTAINERS")

def _matrix_test_impl(ctx):
    env = unittest.begin(ctx)

    # PPC + GCC + linker=auto selects LLD, which runs on the ARM64 host.
    # Its directory must be passed through -B even on hosts without LLVM.
    asserts.true(env, "external/{toolchain_repo_name}/stow/llvm-v5/bin" in COMMON_BINDIRS)
    for arch, toolchains, containers in [
        ("s390x", S390X_GCC_TOOLCHAINS, S390X_SYSROOT_CONTAINERS),
        ("ppc64le", PPC64LE_GCC_TOOLCHAINS, PPC64LE_SYSROOT_CONTAINERS),
    ]:
        asserts.equals(env, ["rhel8", "rhel9", "rhel10"], toolchains.keys())
        asserts.equals(env, toolchains.keys(), containers.keys())
        for distro, info in toolchains.items():
            asserts.equals(env, "{}_{}_on_{}_aarch64".format(distro, arch, distro), cross_selector(distro, arch))
            asserts.equals(env, 64, len(info["sha"]))
            asserts.true(env, "arm64" in info["url"])
            asserts.true(env, info["url"].startswith("https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/"))
            asserts.false(env, "patch" in info["url"])
            asserts.true(env, "@sha256:" in containers[distro])
    return unittest.end(env)

matrix_test = unittest.make(_matrix_test_impl)
