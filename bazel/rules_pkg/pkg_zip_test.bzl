load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@internal_platforms_do_not_use//host:constraints.bzl", "HOST_CONSTRAINTS")
load("@rules_pkg//pkg:zip.bzl", "pkg_zip")
load("//bazel:skylib_test.bzl", "skylib_test_attrs", "skylib_test_end")

def _source_inputs_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [action for action in analysistest.target_actions(env) if action.mnemonic == "PackageZip"]
    asserts.equals(env, 1, len(actions))
    if len(actions) != 1:
        return skylib_test_end(env)

    inputs = actions[0].inputs.to_list()
    basenames = [file.basename for file in inputs]
    runfiles_inputs = [file.path for file in inputs if ".venv" in file.path or ".runfiles" in file.path]
    if ctx.attr.source_only:
        for source in ["build_zip.py", "build_info.py", "manifest.py", "helpers.py"]:
            asserts.true(env, source in basenames, "the Linux ZIP runner needs " + source)
        asserts.equals(env, [], runfiles_inputs, "the Linux ZIP runner must not stage launcher runfiles or virtualenv trees")
        asserts.true(env, "build_zip.exe" in basenames if ctx.attr.windows_launcher else "build_zip" in basenames, "the launcher must remain an explicit action input")
    else:
        asserts.true(env, len(runfiles_inputs) > 0, "native execution needs the launcher runfiles tree")
    if ctx.attr.windows_launcher:
        asserts.true(env, actions[0].argv[0].endswith("build_zip.exe"), "the Windows fixture must use the production Windows launcher")
    return skylib_test_end(env)

_source_inputs_test = analysistest.make(_source_inputs_test_impl, attrs = skylib_test_attrs() | {
    "source_only": attr.bool(),
    "windows_launcher": attr.bool(default = False),
})
_source_inputs_windows_test = analysistest.make(_source_inputs_test_impl, attrs = skylib_test_attrs(windows = True) | {
    "source_only": attr.bool(),
    "windows_launcher": attr.bool(default = True),
})

def pkg_zip_test_suite(name):
    for windows in [False, True]:
        # Constrain the ZIP fixture itself, since constraints on the analysis
        # test do not affect target_under_test's launcher/toolchain selection.
        exec_constraints = ["@platforms//os:windows"] if windows else HOST_CONSTRAINTS
        compatibility = ["@platforms//os:windows"] if windows else select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
        test_rule = _source_inputs_windows_test if windows else _source_inputs_test
        for source_only in [True, False]:
            test_name = name + ("" if source_only else "_native") + ("_windows_test" if windows else "")
            pkg_zip(
                name = test_name + "_input",
                srcs = [Label("//bazel/install_rules:msi_test.wxs")],
                build_zip_source_only = source_only,
                exec_compatible_with = exec_constraints,
                target_compatible_with = compatibility,
                testonly = True,
                tags = ["manual"],
            )
            test_rule(
                name = test_name,
                target_under_test = ":" + test_name + "_input",
                source_only = source_only,
                exec_compatible_with = exec_constraints,
                target_compatible_with = compatibility,
                tags = ["mongo_unittest"],
            )
