"""Keep target sysroots separate from the native execution toolchain."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":sysroot_dump.bzl", "sysroot_container_cli_error", "sysroot_dump_disabled_reason")

def _rhel9_os_release(arguments):
    return struct(return_code = 0, stdout = "Red Hat Enterprise Linux\n9.3\n")

def _al2023_os_release(arguments):
    if arguments[-1] == "/etc/system-release":
        return struct(return_code = 0, stdout = "")
    return struct(return_code = 0, stdout = "Amazon Linux\n2023\n")

def _context(selector = "", use_sysroot = "1", execute = _rhel9_os_release):
    return struct(
        os = struct(
            name = "linux",
            arch = "aarch64",
            environ = {
                "USE_RBE_SYSROOT": use_sysroot,
                "MONGO_LINUX_CROSS_TOOLCHAIN": selector,
            },
        ),
        execute = execute,
    )

def _native_sysroot_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.true(env, "--repo_env=USE_RBE_SYSROOT=0" in sysroot_container_cli_error())

    # --config=local sets USE_RBE_SYSROOT even when the target is cross-built.
    # Native host tools must continue to use the RHEL host's own filesystem.
    for distro in ["rhel8", "rhel9", "rhel10"]:
        selector = "{}_s390x_on_{}_aarch64".format(distro, distro)
        asserts.equals(
            env,
            "only enabled on Amazon Linux 2023 hosts (host distro: rhel9)",
            sysroot_dump_disabled_reason(_context(selector)),
        )

    # Cross selection must not disable an otherwise supported native sysroot.
    asserts.equals(
        env,
        None,
        sysroot_dump_disabled_reason(_context("rhel9_s390x_on_rhel9_aarch64", execute = _al2023_os_release)),
    )
    asserts.equals(env, "USE_RBE_SYSROOT is not set", sysroot_dump_disabled_reason(_context(use_sysroot = "0")))
    return unittest.end(env)

native_sysroot_test = unittest.make(_native_sysroot_test_impl)

def _cross_sysroot_test_impl(ctx):
    env = unittest.begin(ctx)
    error = sysroot_container_cli_error(cross = True)
    asserts.true(env, "cross build configuration" in error)
    asserts.true(env, "MONGO_LINUX_CROSS_TOOLCHAIN" in error)
    asserts.false(env, "USE_RBE_SYSROOT" in error)
    for distro in ["rhel8", "rhel9", "rhel10"]:
        selector = "{}_s390x_on_{}_aarch64".format(distro, distro)

        # The cross sysroot is required even without the optional native flag.
        asserts.equals(env, None, sysroot_dump_disabled_reason(_context(selector, "0"), cross = True))
    for distro in ["rhel8", "rhel9", "rhel10"]:
        for arch in ["s390x", "ppc64le"]:
            selector = "{}_{}_on_{}_aarch64".format(distro, arch, distro)
            asserts.equals(env, None, sysroot_dump_disabled_reason(_context(selector, "0"), cross = True, target_arch = arch))
            other_arch = "s390x" if arch == "ppc64le" else "ppc64le"
            asserts.equals(
                env,
                "cross selector targets {}, not {}".format(arch, other_arch),
                sysroot_dump_disabled_reason(_context(selector, "0"), cross = True, target_arch = other_arch),
            )
    asserts.equals(
        env,
        "MONGO_LINUX_CROSS_TOOLCHAIN is not set",
        sysroot_dump_disabled_reason(_context(), cross = True),
    )
    asserts.equals(
        env,
        "invalid MONGO_LINUX_CROSS_TOOLCHAIN value 'invalid'",
        sysroot_dump_disabled_reason(_context("invalid"), cross = True),
    )
    return unittest.end(env)

cross_sysroot_test = unittest.make(_cross_sysroot_test_impl)
