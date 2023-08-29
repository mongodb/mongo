"""Plumbing necessary to be able to configure compiler"""

compiler_type_provider = provider("<required description>", fields = ["type"])

compiler_type = rule(
    implementation = lambda ctx: compiler_type_provider(type = ctx.build_setting_value),
    build_setting = config.string(flag = True),
)
