"""Unit tests for the Linux cross toolchain repository rule helpers."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":mongo_linux_cross_toolchain.bzl", "execs_for_target", "linux_cross_toolchain_repo_names")

def _execs_for_target_test_impl(ctx):
    env = unittest.begin(ctx)

    # The same-release filter pairs every target RHEL release only with
    # execution platforms of the same release: the composed cross repository
    # needs target and execution archives from one release, and the wrapper
    # keeps the host action container on that release too.
    for target_distro in ("rhel8", "rhel9", "rhel10"):
        asserts.equals(
            env,
            [
                (target_distro, "x86_64"),
                (target_distro, "aarch64"),
            ],
            execs_for_target(target_distro),
        )

    # A distro without execution platforms yields nothing rather than pairing
    # with a foreign release.
    asserts.equals(env, [], execs_for_target("rhel11"))

    return unittest.end(env)

_execs_for_target_test = unittest.make(_execs_for_target_test_impl)

def _expected_repo_names():
    return [
        "mongo_linux_cross_toolchain_v5_{}_{}_on_{}_{}".format(
            target_distro,
            target_arch,
            exec_distro,
            exec_arch,
        )
        for target_distro, target_arch in [
            ("rhel8", "ppc64le"),
            ("rhel8", "s390x"),
            ("rhel9", "ppc64le"),
            ("rhel9", "s390x"),
            ("rhel10", "ppc64le"),
            ("rhel10", "s390x"),
        ]
        for exec_distro, exec_arch in execs_for_target(target_distro)
    ]

def _repo_names_test_impl(ctx):
    env = unittest.begin(ctx)

    expected = _expected_repo_names()
    asserts.equals(env, 12, len(expected))
    asserts.equals(env, expected, linux_cross_toolchain_repo_names())

    # MODULE.bazel's use_repo and register_toolchains calls, and the .bazelrc
    # cross configs, reference these generated repository names. A rename that
    # breaks the same-release shape (or drops an execution architecture) fails
    # here before it fails repository hydration.
    for name in linux_cross_toolchain_repo_names():
        parts = name.split("_on_")
        asserts.true(env, len(parts) == 2, name)
        target_distro = parts[0].split("mongo_linux_cross_toolchain_v5_")[1].split("_")[0]
        exec_distro = parts[1].split("_")[0]
        asserts.equals(env, target_distro, exec_distro, name)
        asserts.true(env, "_x86_64" in name or "_aarch64" in name, name)

    return unittest.end(env)

_repo_names_test = unittest.make(_repo_names_test_impl)

def mongo_linux_cross_toolchain_test_suite(name):
    """Test the cross toolchain repository matrix without downloading archives.

    Args:
        name: Name of the test suite.
    """
    unittest.suite(
        name,
        _execs_for_target_test,
        _repo_names_test,
    )
