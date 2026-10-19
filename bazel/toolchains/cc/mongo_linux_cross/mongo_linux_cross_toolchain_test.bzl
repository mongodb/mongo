"""Ensure every cross compiler/runtime pair stays on its own RHEL release."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load(":mongo_linux_cross_toolchain.bzl", "cross_selector")
load(":mongo_toolchain_flags_v5.bzl", "COMMON_BINDIRS", "cross_lld_bin_dir", "cross_lld_files")
load(":mongo_toolchain_version_v5.bzl", "PPC64LE_GCC_TOOLCHAINS", "S390X_GCC_TOOLCHAINS")
load(":sysroot_containers.bzl", "PPC64LE_SYSROOT_CONTAINERS", "S390X_SYSROOT_CONTAINERS")

def _matrix_test_impl(ctx):
    env = unittest.begin(ctx)

    # PPC + GCC + linker=auto selects LLD, which runs on the ARM64 host.
    # Its directory must be passed through -B even on hosts without LLVM.
    asserts.true(env, "{lld_bin_dir}" in COMMON_BINDIRS)
    asserts.equals(env, "external/mongo_toolchain_v5/stow/llvm-v5/bin/", cross_lld_bin_dir("ppc64le", "ppc_repo"))
    asserts.equals(env, ["@mongo_toolchain_v5//:lld_files"], cross_lld_files("ppc64le"))
    asserts.equals(env, "external/s390x_repo/stow/llvm-v5/bin/", cross_lld_bin_dir("s390x", "s390x_repo"))
    asserts.equals(env, [], cross_lld_files("s390x"))
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

def _ppc_lld_link_test_impl(ctx):
    env = analysistest.begin(ctx)
    link_actions = [action for action in analysistest.target_actions(env) if action.mnemonic == "CppLink"]
    asserts.equals(env, 1, len(link_actions))
    if link_actions:
        action = link_actions[0]
        asserts.true(env, "-fuse-ld=lld" in action.argv)
        asserts.true(env, cross_lld_bin_dir("ppc64le", "unused") in action.argv)
        inputs = [file.path for file in action.inputs.to_list()]
        for executable in ["ld.lld", "lld"]:
            asserts.true(env, "external/mongo_toolchain_v5/stow/llvm-v5/bin/" + executable in inputs)
    return analysistest.end(env)

# Run with --config=opt and a linux-ppc64le-rhel*-cross-gcc-arm64 configuration.
ppc_lld_link_test = analysistest.make(_ppc_lld_link_test_impl)
