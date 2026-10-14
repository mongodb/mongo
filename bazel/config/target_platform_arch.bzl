"""Resolve build metadata from target platform constraints."""

TARGET_ARCH_CONSTRAINTS = {
    "_arch_x86_64": attr.label(default = "@platforms//cpu:x86_64"),
    "_arch_aarch64": attr.label(default = "@platforms//cpu:aarch64"),
    "_arch_arm64": attr.label(default = "@platforms//cpu:arm64"),
    "_arch_ppc": attr.label(default = "@platforms//cpu:ppc"),
    "_arch_ppc64le": attr.label(default = "@platforms//cpu:ppc64le"),
    "_arch_s390x": attr.label(default = "@platforms//cpu:s390x"),
    "_arch_wasm32": attr.label(default = "@platforms//cpu:wasm32"),
    "_arch_macos": attr.label(default = "@platforms//os:macos"),
    "_arch_windows": attr.label(default = "@platforms//os:windows"),
}

def target_platform_arch(ctx):
    """Preserve platform.machine() spellings without inspecting the execution host."""
    is_macos = ctx.target_platform_has_constraint(ctx.attr._arch_macos[platform_common.ConstraintValueInfo])
    is_windows = ctx.target_platform_has_constraint(ctx.attr._arch_windows[platform_common.ConstraintValueInfo])
    architectures = [
        (ctx.attr._arch_x86_64, "amd64" if is_windows else "x86_64"),
        (ctx.attr._arch_aarch64, "arm64" if is_macos or is_windows else "aarch64"),
        (ctx.attr._arch_arm64, "arm64" if is_macos or is_windows else "aarch64"),
        (ctx.attr._arch_ppc, "ppc64le"),
        (ctx.attr._arch_ppc64le, "ppc64le"),
        (ctx.attr._arch_s390x, "s390x"),
        (ctx.attr._arch_wasm32, "wasm32"),
    ]
    for constraint, architecture in architectures:
        if ctx.target_platform_has_constraint(constraint[platform_common.ConstraintValueInfo]):
            return architecture
    fail("Unable to determine target architecture from the target platform")
