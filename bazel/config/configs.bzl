"""Starlark bazel build configurations, see https://bazel.build/extending/config"""

# =============
# compiler_type
# =============

compiler_type_provider = provider(
    doc = "Select the compiler (e.g.: gcc)",
    fields = {"compiler_type": "Choose one of [gcc, clang]"},
)

compiler_type = rule(
    implementation = lambda ctx: compiler_type_provider(compiler_type = ctx.build_setting_value),
    build_setting = config.string(flag = True),
)

# ==========
# build_mode
# ==========

build_mode_values = ["dbg", "release", "opt_on", "opt_off", "opt_size", "opt_debug"]

build_mode_provider = provider(
    doc = "Select the overall mode of the build, e.g debug/optimized or some combination/extension of those.",
    fields = {"build_mode": "choose one of " + ".".join(build_mode_values)},
)

def build_mode_impl(ctx):
    build_mode_value = ctx.build_setting_value
    if build_mode_value not in build_mode_values:
        fail(str(ctx.label) + " build_mode allowed to take values {" + ", ".join(build_mode_values) + "} but was set to unallowed value " + build_mode_value)
    return build_mode_provider(build_mode = build_mode_value)

build_mode = rule(
    implementation = build_mode_impl,
    build_setting = config.string(flag = True),
)

# =========
# libunwind
# =========

use_libunwind_provider = provider(fields = ["type"])

use_libunwind = rule(
    implementation = lambda ctx: use_libunwind_provider(type = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)
