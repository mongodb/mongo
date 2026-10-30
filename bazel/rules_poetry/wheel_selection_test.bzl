"""Regression tests for Poetry's hosted-wheel and marker handling."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load("@rules_poetry//rules_poetry:poetry.bzl", "collect_package_variants", "lock_marker", "locked_requirement", "package_marker")

def _variants_test_impl(ctx):
    env = unittest.begin(ctx)
    packages = [
        {"name": "cryptography", "version": "44.0.2", "markers": "platform_machine != 's390x' and platform_machine != 'ppc64le'", "files": [{"hash": "sha256:pypi"}]},
        {"name": "cryptography", "version": "44.0.2", "markers": "platform_machine == 's390x'", "source": {"type": "url", "url": "https://example.com/crypto-s390x.whl"}, "files": [{"hash": "sha256:s390x"}]},
        {"name": "cryptography", "version": "44.0.2", "markers": "platform_machine == 'ppc64le'", "source": {"type": "url", "url": "https://example.com/crypto-ppc64le.whl"}, "files": [{"hash": "sha256:ppc64le"}]},
    ]
    variants = collect_package_variants(packages, {})
    asserts.equals(env, 1, len(variants))
    asserts.equals(env, packages, variants[0])
    requirements = [locked_requirement(package, []) for package in variants[0]]
    asserts.equals(env, "cryptography @ https://example.com/crypto-s390x.whl ; platform_machine == 's390x' --hash=sha256:s390x", requirements[1])
    asserts.equals(env, "cryptography @ https://example.com/crypto-ppc64le.whl ; platform_machine == 'ppc64le' --hash=sha256:ppc64le", requirements[2])
    asserts.equals(env, "cryptography==44.0.2 ; platform_machine != 's390x' and platform_machine != 'ppc64le' --hash=sha256:pypi", requirements[0])
    asserts.equals(env, "(platform_machine != 's390x' and platform_machine != 'ppc64le') or (platform_machine == 's390x') or (platform_machine == 'ppc64le')", package_marker(packages))
    asserts.equals(env, "", package_marker([packages[0], {"name": "cryptography"}]))
    asserts.equals(env, [], collect_package_variants(packages, {"cryptography": True}))
    asserts.equals(env, "(sys_platform == 'win32') or (sys_platform == 'darwin')", lock_marker({"markers": {"group1": "sys_platform == 'win32'", "group2": "sys_platform == 'darwin'"}}))
    return unittest.end(env)

variants_test = unittest.make(_variants_test_impl)

def _download_args_test_impl(ctx):
    env = analysistest.begin(ctx)
    downloads = [action for action in analysistest.target_actions(env) if action.mnemonic == "DownloadWheel"]
    asserts.equals(env, 1, len(downloads))
    argv = downloads[0].argv
    asserts.true(env, argv[1].endswith("/python_build_environment.py"))
    asserts.equals(env, "pip", argv[2])
    asserts.true(env, any([file.path == argv[1] for file in downloads[0].inputs.to_list()]))
    asserts.equals(env, ctx.attr.expected_wheel_only, "--only-binary" in argv)
    if ctx.attr.expected_wheel_only:
        asserts.equals(env, ":all:", argv[argv.index("--only-binary") + 1])
    asserts.true(env, "--require-hashes" in argv)
    asserts.true(env, "--no-deps" in argv)
    return analysistest.end(env)

download_args_test = analysistest.make(
    _download_args_test_impl,
    attrs = {"expected_wheel_only": attr.bool()},
)

def _direct_source_rejected_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "cryptography requires a prebuilt wheel; direct source URLs are disabled")
    return analysistest.end(env)

direct_source_rejected_test = analysistest.make(_direct_source_rejected_test_impl, expect_failure = True)

def _install_environment_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [action for action in analysistest.target_actions(env) if action.mnemonic == "CopyWheel"]
    asserts.equals(env, 1, len(actions))
    argv = actions[0].argv
    asserts.true(env, argv[1].endswith("/python_build_environment.py"))
    asserts.equals(env, ["pip", "install"], argv[2:4])
    asserts.true(env, any([file.path == argv[1] for file in actions[0].inputs.to_list()]))
    return analysistest.end(env)

install_environment_test = analysistest.make(_install_environment_test_impl)
