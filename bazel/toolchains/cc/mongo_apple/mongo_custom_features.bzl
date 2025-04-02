load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
)

_OBJCPP_EXECUTABLE_ACTION_NAME = "objc++-executable"

_DYNAMIC_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ACTION_NAMES.objc_executable,
    _OBJCPP_EXECUTABLE_ACTION_NAME,
]

mongo_frameworks_feature = feature(
    name = "mongo_frameworks",
    enabled = True,
    flag_sets = [
        flag_set(
            actions = _DYNAMIC_LINK_ACTIONS,
            flag_groups = [
                flag_group(
                    flags = ["-framework", "CoreFoundation", "-framework", "Security"],
                ),
            ],
        ),
    ],
)
