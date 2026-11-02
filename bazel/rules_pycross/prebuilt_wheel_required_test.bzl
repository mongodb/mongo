"""Verify the source fallback stops during analysis."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _source_build_disabled_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "cryptography requires a compatible prebuilt wheel in uv.lock; source builds are disabled")
    return analysistest.end(env)

source_build_disabled_test = analysistest.make(
    _source_build_disabled_test_impl,
    expect_failure = True,
)
