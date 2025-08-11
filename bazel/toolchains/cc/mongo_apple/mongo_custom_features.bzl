load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
)
load(
    "//bazel/toolchains/cc:mongo_custom_features.bzl",
    "all_compile_actions",
    "get_common_features",
)

_OBJCPP_EXECUTABLE_ACTION_NAME = "objc++-executable"

_DYNAMIC_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
    ACTION_NAMES.objc_executable,
    _OBJCPP_EXECUTABLE_ACTION_NAME,
]

def get_apple_features(ctx):
    """ get_features returns a list of toolchain features for apple platform.

    The list of features that is returned is a combined list of
    common features and apple specific features.

    Args:
        ctx: The toolchain context.

    Returns:
        list: The list of features.
    """

    return get_common_features(ctx) + [
        feature(
            name = "mongo_preprocessor_defines",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = [
                        ACTION_NAMES.preprocess_assemble,
                        ACTION_NAMES.c_compile,
                        ACTION_NAMES.cpp_compile,
                        ACTION_NAMES.cpp_header_parsing,
                        ACTION_NAMES.cpp_module_compile,
                        ACTION_NAMES.linkstamp_compile,
                        ACTION_NAMES.objc_compile,
                        ACTION_NAMES.objcpp_compile,
                    ],
                ),
            ],
        ),
        feature(
            name = "macos_general_warnings",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                # As of XCode 9, this flag must be present (it is not enabled by -Wall),
                                # in order to enforce that -mXXX-version-min=YYY will enforce that you
                                # don't use APIs from ZZZ.
                                "-Wunguarded-availability",
                                "-Wno-enum-constexpr-conversion",
                            ],
                        ),
                    ],
                ),
            ],
        ),

        # Enable sized deallocation support.
        #
        # Bazel doesn't allow for defining C++-only flags without a custom toolchain
        # config. This is setup in the Linux toolchain, but currently there is no custom
        # MacOS toolchain. Enabling warnings-as-errors will fail the build if this flag
        # is passed to the compiler when building C code. Define it here on MacOS only
        # to allow us to configure warnings-as-errors on Linux.
        #
        # TODO(SERVER-90183): Remove this once custom toolchain configuration is
        #                     implemented on MacOS.
        feature(
            name = "macos_fsized_deallocation",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-fsized-deallocation",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "macos_general_link_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = _DYNAMIC_LINK_ACTIONS,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-lresolv",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "macos_mongo_frameworks",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = _DYNAMIC_LINK_ACTIONS,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-framework",
                                "CoreFoundation",
                                "-framework",
                                "Security",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "macos_no_deduplicate",
            enabled = ctx.attr.optimization_level == "O0",
            flag_sets = [
                flag_set(
                    actions = _DYNAMIC_LINK_ACTIONS,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-Wl,-no_deduplicate",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "macos_standard_c_and_c_plus_plus",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = [
                        ACTION_NAMES.assemble,
                        ACTION_NAMES.preprocess_assemble,
                        ACTION_NAMES.linkstamp_compile,
                        ACTION_NAMES.cpp_compile,
                        ACTION_NAMES.cpp_header_parsing,
                        ACTION_NAMES.cpp_module_compile,
                        ACTION_NAMES.cpp_module_codegen,
                        ACTION_NAMES.lto_backend,
                        ACTION_NAMES.clif_match,
                        ACTION_NAMES.objcpp_compile,
                    ],
                    flag_groups = [flag_group(flags = ["-std=c++20"])],
                ),
                flag_set(
                    actions = [
                        ACTION_NAMES.c_compile,
                        ACTION_NAMES.objc_compile,
                    ],
                    flag_groups = [flag_group(flags = ["-std=c17"])],
                ),
            ],
        ),
        # TODO(SERVER-105741): feature g0, g1, g2, g3 were copied from ../mongo_linux/mongo_linux_cc_toolchain_config.bzl file.
        # Since we are combining the Linux and Apple toolchains config together, the merging of the
        # debug_level feature will be done at the same time. Need to pay attention to the disable_debug_symbols
        # attribute and other implied features in mongo_linux_cc_toolchain_config.bzl file during the merge.
        feature(
            name = "g0",
            enabled = ctx.attr.debug_level == 0,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-g0",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "g1",
            enabled = ctx.attr.debug_level == 1,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-g1",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "g2",
            enabled = ctx.attr.debug_level == 2,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-g2",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "g3",
            enabled = ctx.attr.debug_level == 3,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                "-g3",
                            ],
                        ),
                    ],
                ),
            ],
        ),
    ]
