""" This file contains customized features that is common to most platforms"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
)

# Features listed in this file is only visible to the toolchain cc.
#visibility([
#    "//",
#    "//bazel/toolchains/cc/mongo_linux",
#    "//bazel/toolchains/cc/mongo_apple",
#])

all_c_compile_actions = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.c_compile,
    ACTION_NAMES.objc_compile,
    ACTION_NAMES.preprocess_assemble,
]

all_cpp_compile_actions = [
    ACTION_NAMES.clif_match,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.objcpp_compile,
]

all_compile_actions = \
    all_c_compile_actions + \
    all_cpp_compile_actions + \
    [
        ACTION_NAMES.lto_backend,
    ]

FEATURES_ATTR_NAMES = struct(
    OPT_LEVEL = "optimization_level",
)

def get_common_features_attrs():
    """ get_common_features_attrs returns a map of attributes and their values.

    The map of attributes settings and their values is returned which
    is used by the toolchain config. Please make sure to refer the key
    by the FEATURE_ATTR_NAMES.

    Returns:
        list: The map of attributes and their values.
    """
    return {
        FEATURES_ATTR_NAMES.OPT_LEVEL: select({
            # This is opt=debug, not to be confused with (opt=on && dbg=on)
            "@//bazel/config:gcc_or_clang_opt_debug": "Og",
            "@//bazel/config:gcc_or_clang_opt_off": "O0",
            "@//bazel/config:gcc_or_clang_opt_on": "O2",
            "@//bazel/config:gcc_or_clang_opt_size": "Os",
            "@//conditions:default": None,
        }),
    }

def get_common_features(ctx):
    """ get_features returns a list of toolchain features.

    The list of features that is returned is a common list
    to most of the platforms that we currently support.

    Args:
        ctx: The toolchain context.

    Returns:
        list: The list of features.
    """
    optimization_level_g_feature = feature(
        name = "og",
        enabled = ctx.attr.optimization_level == "Og",
        provides = [
            "O0",
            "O2",
            "Os",
        ],
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Og",
                        ],
                    ),
                ],
            ),
        ],
    )

    optimization_level_0_feature = feature(
        name = "o0",
        enabled = ctx.attr.optimization_level == "O0",
        provides = [
            "Og",
            "O2",
            "Os",
        ],
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-O0",
                        ],
                    ),
                ],
            ),
        ],
    )

    optimization_level_2_feature = feature(
        name = "o2",
        enabled = ctx.attr.optimization_level == "O2",
        provides = [
            "Og",
            "O1",
            "Os",
        ],
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-O2",
                        ],
                    ),
                ],
            ),
        ],
    )

    optimization_level_size_feature = feature(
        name = "os",
        enabled = ctx.attr.optimization_level == "Os",
        provides = [
            "Og",
            "O1",
            "O2",
        ],
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Os",
                        ],
                    ),
                ],
            ),
        ],
    )

    return [
        optimization_level_g_feature,
        optimization_level_0_feature,
        optimization_level_2_feature,
        optimization_level_size_feature,
    ]
