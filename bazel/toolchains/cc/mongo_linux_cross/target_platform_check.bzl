"""Reject smoke builds before compilation if Bazel selected a native platform."""

def _s390x_target_platform_check_impl(ctx):
    if not ctx.target_platform_has_constraint(ctx.attr._s390x[platform_common.ConstraintValueInfo]):
        fail("The cross smoke build requires an s390x target platform")
    return []

s390x_target_platform_check = rule(
    implementation = _s390x_target_platform_check_impl,
    attrs = {"_s390x": attr.label(default = "@platforms//cpu:s390x")},
)

def _s390x_rust_toolchain_check_impl(ctx):
    triple = ctx.toolchains["@rules_rust//rust:toolchain_type"].target_triple.str
    if triple != "s390x-unknown-linux-gnu":
        fail("Expected an s390x Rust target toolchain, got {}".format(triple))
    return []

s390x_rust_toolchain_check = rule(
    implementation = _s390x_rust_toolchain_check_impl,
    toolchains = ["@rules_rust//rust:toolchain_type"],
)
