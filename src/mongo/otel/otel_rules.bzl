"""Common otel-specific bazel build rules intended to be used in individual BUILD files in the
"otel" module.
"""

OTEL_TARGET_COMPATIBLE_WITH = select({
    "//bazel/config:build_otel_enabled": [],
    "//conditions:default": ["@platforms//:incompatible"],
})
