"""Runfiles support for Skylib tests executed through MongoDB's test launchers."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest")

def skylib_test_end(env):
    """Stage the launcher alongside Bazel's generated analysis-test executable."""
    launcher = env.ctx.attr._test_launcher[DefaultInfo]
    runfiles = env.ctx.runfiles(transitive_files = launcher.files).merge(launcher.default_runfiles)
    return analysistest.end(env) + [DefaultInfo(runfiles = runfiles)]

def skylib_test_attrs(windows = False):
    """Select the launcher matching the test's execution platform."""
    return {"_test_launcher": attr.label(
        default = Label("//bazel:windows_test_launcher" if windows else "//bazel:test_wrapper"),
        cfg = "exec",
    )}
