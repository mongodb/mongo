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

COMPILERS = struct(
    CLANG = "clang",
    GCC = "gcc",
)

LINKERS = struct(
    GOLD = "gold",
    LLD = "lld",
    MOLD = "mold",
)

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
    return [
        feature(
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
        ),
        feature(
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
        ),
        feature(
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
        ),
        feature(
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
        ),
        feature(
            name = "no_unused_function",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                # Clang likes to warn about unused functions, which seems a tad
                                # aggressive and breaks -Werror, which we want to be able to use.
                                "-Wno-unused-function",
                            ],
                        ),
                    ],
                ),
            ],
        ),
        feature(
            name = "no_defaulted_function_deleted",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(
                            flags = [
                                # This warning was added in Apple clang version 11 and flags many
                                # explicitly defaulted move constructors and assignment operators for
                                # being implicitly deleted, which is not useful.
                                "-Wno-defaulted-function-deleted",
                            ],
                        ),
                    ],
                ),
            ],
        ),

        # -Wno-invalid-offsetof is only valid for C++ but not for C
        feature(
            name = "no_invalid_offsetof_warning",
            enabled = False,
            flag_sets = [
                flag_set(
                    actions = all_cpp_compile_actions,
                    flag_groups = [flag_group(flags = ["-Wno-invalid-offsetof"])],
                ),
            ],
        ),
        feature(
            name = "no_unknown_pragmas",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Do not warn on unknown pragmas.
                        "-Wno-unknown-pragmas",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_builtin_macro_redefined",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Replace compile timestamp-related macros for reproducible binaries with consistent hashes.
                        "-Wno-builtin-macro-redefined",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_unused_local_typedefs",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # This warning was added in g++-4.8.
                        "-Wno-unused-local-typedefs",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_unused_lambda_capture",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # This warning was added in clang-5 and flags many of our lambdas. Since
                        # it isn't actively harmful to capture unused variables we are
                        # suppressing for now with a plan to fix later.
                        "-Wno-unused-lambda-capture",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_deprecated_declarations",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Prevents warning about using deprecated features (such as auto_ptr in
                        # c++11) Using -Wno-error=deprecated-declarations does not seem to work
                        # on some compilers, including at least g++-4.6.
                        "-Wno-deprecated-declarations",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_unused_const_variable",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # New in clang-3.4, trips up things mostly in third_party, but in a few
                        # places in the primary mongo sources as well.
                        "-Wno-unused-const-variable",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_deprecate_this_capture",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        "-Wno-deprecated-this-capture",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_undefined_var_template",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Disable warning about templates that can't be implicitly instantiated.
                        # It is an attempt to make a link error into an easier-to-debug compiler
                        # failure, but it triggers false positives if explicit instantiation is
                        # used in a TU that can see the full definition. This is a problem at
                        # least for the S2 headers.
                        "-Wno-undefined-var-template",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_unused_private_field",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Clang likes to warn about unused private fields, but some of our
                        # third_party libraries have such things.
                        "-Wno-unused-private-field",
                    ])],
                ),
            ],
        ),
        feature(
            name = "no_potentially_evaluated_expression",
            enabled = ctx.attr.compiler == COMPILERS.CLANG,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(flags = [
                        # Don't issue warnings about potentially evaluated expressions
                        "-Wno-potentially-evaluated-expression",
                    ])],
                ),
            ],
        ),
    ]
