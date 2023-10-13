"""Starlark bazel build configurations, see https://bazel.build/extending/config"""

compiler_type_provider = provider("<required description>", fields = ["type"])

compiler_type = rule(
    implementation = lambda ctx: compiler_type_provider(type = ctx.build_setting_value),
    build_setting = config.string(flag = True),
)

use_libunwind_provider = provider(fields = ["type"])

use_libunwind = rule(
    implementation = lambda ctx: use_libunwind_provider(type = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)
