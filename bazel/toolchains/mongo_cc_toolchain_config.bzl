"""This module provides the cc_toolchain_config rule."""

load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "action_config",
    "feature",
    "flag_group",
    "flag_set",
    "tool",
    "tool_path",
    "with_feature_set",
)
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

all_compile_actions = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
    ACTION_NAMES.clif_match,
    ACTION_NAMES.lto_backend,
]

all_cpp_compile_actions = [
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
    ACTION_NAMES.clif_match,
]

all_c_compile_actions = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
]

preprocessor_compile_actions = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.clif_match,
]

all_link_actions = [
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

lto_index_actions = [
    ACTION_NAMES.lto_index_for_executable,
    ACTION_NAMES.lto_index_for_dynamic_library,
    ACTION_NAMES.lto_index_for_nodeps_dynamic_library,
]

def _impl(ctx):
    action_configs = [
        action_config(action_name = "objcopy_embed_data", tools = [tool(path = ctx.attr.tool_paths["objcopy"])]),
    ] + [
        action_config(action_name = ACTION_NAMES.llvm_cov, tools = [tool(path = ctx.attr.tool_paths["llvm-cov"])]),
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(path = ctx.attr.tool_paths["gcc"])])
        for name in all_c_compile_actions
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(path = ctx.attr.tool_paths["g++"])])
        for name in all_cpp_compile_actions
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(path = ctx.attr.tool_paths["g++"])])
        for name in all_link_actions
    ]

    opt_feature = feature(name = "opt")
    dbg_feature = feature(name = "dbg")

    unfiltered_compile_flags_feature = feature(
        name = "unfiltered_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            # Do not resolve our symlinked resource prefixes to real paths. This is required to
                            # make includes resolve correctly.
                            "-no-canonical-prefixes",

                            # Replace compile timestamp-related macros for reproducible binaries with consistent hashes.
                            "-Wno-builtin-macro-redefined",
                        ],
                    ),
                ],
            ),
        ],
    )

    omitted_timestamps_feature = feature(
        name = "redacted_dates",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-D__DATE__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
                            "-D__TIMESTAMP__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
                            "-D__TIME__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
                        ],
                    ),
                ],
            ),
        ],
    )

    default_compile_flags_feature = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        # Security hardening requires optimization.
                        # We need to undef it as some distributions now have it enabled by default.
                        flags = ["-U_FORTIFY_SOURCE"],
                    ),
                ],
                with_features = [
                    with_feature_set(
                        not_features = ["thin_lto"],
                    ),
                ],
            ),
            flag_set(
                actions = all_c_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-std=c11",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-std=c++20",
                            "-nostdinc++",
                        ],
                    ),
                ],
            ),
        ],
    )

    include_paths_feature = feature(
        name = "include_paths",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.clif_match,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-iquote", "%{quote_include_paths}"],
                        iterate_over = "quote_include_paths",
                    ),
                    flag_group(
                        flags = ["-I%{include_paths}"],
                        iterate_over = "include_paths",
                    ),
                    flag_group(
                        flags = ["-isystem", "%{system_include_paths}"],
                        iterate_over = "system_include_paths",
                    ),
                ],
            ),
        ],
    )

    library_search_directories_feature = feature(
        name = "library_search_directories",
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-L%{library_search_directories}"],
                        iterate_over = "library_search_directories",
                        expand_if_available = "library_search_directories",
                    ),
                ],
            ),
        ],
    )

    supports_dynamic_linker_feature = feature(
        name = "supports_dynamic_linker",
        enabled = True,
    )

    objcopy_embed_flags_feature = feature(
        name = "objcopy_embed_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = ["objcopy_embed_data"],
                flag_groups = [flag_group(flags = ["-I", "binary"])],
            ),
        ],
    )

    user_compile_flags_feature = feature(
        name = "user_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = ["%{user_compile_flags}"],
                        iterate_over = "user_compile_flags",
                        expand_if_available = "user_compile_flags",
                    ),
                ],
            ),
        ],
    )

    extra_cflags_feature = feature(
        name = "extra_cflags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.c_compile],
                flag_groups = [flag_group(flags = ctx.attr.extra_cflags)],
            ),
        ] if len(ctx.attr.extra_cflags) > 0 else [],
    )

    extra_cxxflags_feature = feature(
        name = "extra_cxxflags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_compile],
                flag_groups = [flag_group(flags = ctx.attr.extra_cxxflags)],
            ),
        ] if len(ctx.attr.extra_cxxflags) > 0 else [],
    )

    includes_feature = feature(
        name = "includes",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-isystem{}".format(include)
                    for include in ctx.attr.includes
                ])],
            ),
        ] if ctx.attr.includes != None and len(ctx.attr.includes) > 0 else [],
    )

    bin_dirs_feature = feature(
        name = "bin_dirs",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-B{}".format(bin_dir)
                    for bin_dir in ctx.attr.bin_dirs
                ])],
            ),
        ] if ctx.attr.bin_dirs != None and len(ctx.attr.bin_dirs) > 0 else [],
    )

    extra_ldflags_feature = feature(
        name = "extra_ldflags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ctx.attr.extra_ldflags)],
            ),
        ] if len(ctx.attr.extra_ldflags) > 0 else [],
    )

    verbose_feature = feature(
        name = "verbose",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["--verbose"])],
            ),
        ] if ctx.attr.verbose else [],
    )

    sysroot_feature = feature(
        name = "sysroot",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.lto_backend,
                    ACTION_NAMES.clif_match,
                ] + all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = ["--sysroot", "%{sysroot}"],
                        expand_if_available = "sysroot",
                    ),
                ],
            ),
        ],
    )

    dependency_file_feature = feature(
        name = "dependency_file",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.objc_compile,
                    ACTION_NAMES.objcpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-MD", "-MF", "%{dependency_file}"],
                        expand_if_available = "dependency_file",
                    ),
                ],
            ),
        ],
    )

    supports_pic_feature = feature(
        name = "supports_pic",
        enabled = False,
    )

    pic_feature = feature(
        name = "pic",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.cpp_module_compile,
                ],
                flag_groups = [
                    flag_group(flags = ["-fPIC"], expand_if_available = "pic"),
                ],
            ),
        ],
    )

    pie_feature = feature(
        name = "pie",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                    ACTION_NAMES.cpp_module_compile,
                ],
                flag_groups = [flag_group(flags = ["-fPIE"])],
            ),
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ["-pie"])],
            ),
        ],
    )

    per_object_debug_info_feature = feature(
        name = "per_object_debug_info",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.assemble,
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_module_codegen,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-gsplit-dwarf", "-g"],
                        expand_if_available = "per_object_debug_info_file",
                    ),
                ],
            ),
        ],
    )

    preprocessor_defines_feature = feature(
        name = "preprocessor_defines",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.preprocess_assemble,
                    ACTION_NAMES.linkstamp_compile,
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                    ACTION_NAMES.cpp_header_parsing,
                    ACTION_NAMES.cpp_module_compile,
                    ACTION_NAMES.clif_match,
                ],
                flag_groups = [
                    flag_group(
                        flags = ["-D%{preprocessor_defines}"],
                        iterate_over = "preprocessor_defines",
                    ),
                ],
            ),
        ],
    )

    features = [
        bin_dirs_feature,
        default_compile_flags_feature,
        include_paths_feature,
        library_search_directories_feature,
        supports_dynamic_linker_feature,
        supports_pic_feature,
        pic_feature,
        pie_feature,
        per_object_debug_info_feature,
        preprocessor_defines_feature,
        objcopy_embed_flags_feature,
        opt_feature,
        dbg_feature,
        user_compile_flags_feature,
        sysroot_feature,
        unfiltered_compile_flags_feature,
        omitted_timestamps_feature,
        extra_cflags_feature,
        extra_cxxflags_feature,
        extra_ldflags_feature,
        includes_feature,
        dependency_file_feature,
        verbose_feature,
    ]

    return [
        cc_common.create_cc_toolchain_config_info(
            abi_libc_version = "local",
            abi_version = "local",
            action_configs = action_configs,
            artifact_name_patterns = [],
            builtin_sysroot = ctx.attr.builtin_sysroot,
            cc_target_os = None,
            compiler = ctx.attr.compiler,
            ctx = ctx,
            cxx_builtin_include_directories = ctx.attr.cxx_builtin_include_directories,
            features = features,
            host_system_name = "local",
            make_variables = [],
            target_cpu = ctx.attr.cpu,
            target_libc = "local",
            target_system_name = "local",
            toolchain_identifier = ctx.attr.toolchain_identifier,
            tool_paths = [
                tool_path(name = name, path = path)
                for name, path in ctx.attr.tool_paths.items()
            ],
        ),
    ]

mongo_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "builtin_sysroot": attr.string(),
        "cxx_builtin_include_directories": attr.string_list(mandatory = True),
        "cpu": attr.string(mandatory = True),
        "compiler": attr.string(mandatory = True),
        "extra_cflags": attr.string_list(mandatory = False),
        "extra_cxxflags": attr.string_list(mandatory = False),
        "extra_ldflags": attr.string_list(mandatory = False),
        "includes": attr.string_list(mandatory = False),
        "bin_dirs": attr.string_list(mandatory = False),
        "tool_paths": attr.string_dict(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "verbose": attr.bool(mandatory = False),
    },
    provides = [CcToolchainConfigInfo],
)
