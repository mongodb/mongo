"""This module provides the cc_toolchain_config rule."""

load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "action_config",
    "env_entry",
    "env_set",
    "feature",
    "flag_group",
    "flag_set",
    "tool",
    "tool_path",
    "with_feature_set",
)
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "//bazel/toolchains/cc:mongo_custom_features.bzl",
    "COMPILERS",
    "LINKERS",
    "all_c_compile_actions",
    "all_compile_actions",
    "all_cpp_compile_actions",
    "get_common_features",
)
load("//bazel/toolchains/cc/mongo_linux:mongo_defines.bzl", "DEFINES")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_flags_v5.bzl", "CLANG_RESOURCE_DIR")

all_non_assembly_compile_actions = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.cpp_header_parsing,
    ACTION_NAMES.cpp_module_compile,
    ACTION_NAMES.cpp_module_codegen,
    ACTION_NAMES.clif_match,
    ACTION_NAMES.lto_backend,
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

    thin_archive_feature = feature(
        name = "thin_archive",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-T",
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
        enabled = (not ctx.attr.linkstatic) or ctx.attr.shared_archive,
    )

    supports_start_end_lib_feature = feature(
        name = "supports_start_end_lib",
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
                actions = [ACTION_NAMES.c_compile, ACTION_NAMES.lto_backend],
                flag_groups = [flag_group(flags = ctx.attr.extra_cflags)],
            ),
        ] if len(ctx.attr.extra_cflags) > 0 else [],
    )

    extra_cxxflags_feature = feature(
        name = "extra_cxxflags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_compile, ACTION_NAMES.lto_backend],
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
                actions = all_compile_actions + all_link_actions + lto_index_actions,
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
                actions = all_link_actions + lto_index_actions,
                flag_groups = [flag_group(flags = ctx.attr.extra_ldflags)],
            ),
        ] if len(ctx.attr.extra_ldflags) > 0 else [],
    )

    verbose_feature = feature(
        name = "verbose",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["--verbose"])],
            ),
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ["-Wl,--verbose"])],
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
        enabled = True,
    )

    default_pic = (not ctx.attr.linkstatic) or ctx.attr.shared_archive

    pic_feature = feature(
        name = "pic",
        enabled = default_pic,
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
                    ACTION_NAMES.lto_backend,
                ],
                flag_groups = [
                    flag_group(flags = ["-fPIC"], expand_if_available = "pic"),
                ],
            ),
        ],
    )

    pie_feature = feature(
        name = "pie",
        enabled = not default_pic,
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
                    ACTION_NAMES.lto_backend,
                ],
                flag_groups = [flag_group(flags = ["-fPIE"])],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [flag_group(flags = ["-pie"])],
            ),
        ],
    )

    per_object_debug_info_feature = feature(
        name = "per_object_debug_info",
        enabled = not ctx.attr.disable_debug_symbols,
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
                with_features = [
                    with_feature_set(
                        not_features = ["disable_debug_symbols"],
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
                    flag_group(
                        flags = [
                            "-D{}".format(preprocessor_define)
                            for preprocessor_define in DEFINES
                        ],
                    ),
                ],
            ),
        ],
    )

    dbg_level_0_feature = feature(
        name = "g0",
        enabled = ctx.attr.debug_level == 0,
        flag_sets = [
            flag_set(
                actions = all_non_assembly_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-g0",
                        ],
                    ),
                ],
            ),
        ],
    )

    dbg_level_1_feature = feature(
        name = "g1",
        enabled = ctx.attr.debug_level == 1 and not ctx.attr.disable_debug_symbols,
        flag_sets = [
            flag_set(
                actions = all_non_assembly_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-g1",
                        ],
                    ),
                ],
            ),
        ],
    )

    dbg_level_2_feature = feature(
        name = "g2",
        enabled = ctx.attr.debug_level == 2 and not ctx.attr.disable_debug_symbols,
        flag_sets = [
            flag_set(
                actions = all_non_assembly_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-g2",
                        ],
                    ),
                ],
                with_features = [
                    with_feature_set(
                        not_features = ["g0", "g1", "g3", "disable_debug_symbols"],
                    ),
                ],
            ),
        ],
    )

    dbg_level_3_feature = feature(
        name = "g3",
        enabled = ctx.attr.debug_level == 3 and not ctx.attr.disable_debug_symbols,
        flag_sets = [
            flag_set(
                actions = all_non_assembly_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-g3",
                        ],
                    ),
                ],
            ),
        ],
    )

    dwarf4_feature = feature(
        name = "dwarf-4",
        enabled = ctx.attr.dwarf_version == 4,
        implies = ["per_object_debug_info"],
        flag_sets = [
            flag_set(
                # This needs to only be set in the cpp compile actions to avoid generating debug info when
                # building assembly files since the assembler doesn't support gdwarf64.
                actions = all_non_assembly_compile_actions,
                flag_groups = [flag_group(flags = ["-gdwarf-4"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
        ],
    )

    dwarf5_feature = feature(
        name = "dwarf-5",
        enabled = ctx.attr.dwarf_version == 5,
        implies = ["per_object_debug_info"],
        flag_sets = [
            flag_set(
                # This needs to only be set in the cpp compile actions to avoid generating debug info when
                # building assembly files since the assembler doesn't support gdwarf64.
                actions = all_non_assembly_compile_actions,
                flag_groups = [flag_group(flags = ["-gdwarf-5"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
        ],
    )

    dwarf32_feature = feature(
        name = "dwarf32",
        # SUSE15 builds system libraries with dwarf32, use dwarf32 to be keep consistent
        enabled = ctx.attr.compiler == COMPILERS.CLANG or ctx.attr.fission or ctx.attr.distro == "suse15",
        implies = ["per_object_debug_info"],
        flag_sets = [
            flag_set(
                # This needs to only be set in the cpp compile actions to avoid generating debug info when
                # building assembly files since the assembler doesn't support gdwarf64.
                actions = all_non_assembly_compile_actions,
                flag_groups = [flag_group(flags = ["-gdwarf32"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ["-gdwarf32"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
        ],
    )

    # gdb crashes with -gsplit-dwarf and -gdwarf64
    dwarf64_feature = feature(
        name = "dwarf64",
        enabled = ctx.attr.compiler == COMPILERS.GCC and not ctx.attr.fission and not ctx.attr.distro == "suse15",
        implies = ["per_object_debug_info"],
        flag_sets = [
            flag_set(
                actions = all_non_assembly_compile_actions,
                flag_groups = [flag_group(flags = ["-gdwarf64"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ["-gdwarf64"])],
                with_features = [with_feature_set(not_features = ["disable_debug_symbols"])],
            ),
        ],
    )

    disable_debug_symbols_feature = feature(
        name = "disable_debug_symbols",
        enabled = ctx.attr.disable_debug_symbols,
        implies = ["g0"],
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                with_features = [
                    with_feature_set(
                        not_features = [
                            "per_object_debug_info",
                            "g1",
                            "g2",
                            "g3",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions,
                with_features = [
                    with_feature_set(
                        not_features = [
                            "per_object_debug_info",
                            "g1",
                            "g2",
                            "g3",
                        ],
                    ),
                ],
            ),
        ],
    )

    # This warning overzealously warns on uses of non-virtual destructors which are benign.
    no_warn_non_virtual_destructor_feature = feature(
        name = "no_warn_non_virtual_destructor",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-non-virtual-dtor"])],
            ),
        ],
    )

    no_deprecated_enum_enum_conversion_feature = feature(
        name = "no_deprecated_enum_enum_conversion",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-deprecated-enum-enum-conversion"])],
            ),
        ],
    )

    no_volatile_feature = feature(
        name = "no_volatile",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-volatile"])],
            ),
        ],
    )

    fsized_deallocation_feature = feature(
        name = "fsized_deallocation",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-fsized-deallocation"])],
            ),
        ],
    )

    # Warn when hiding a virtual function.
    overloaded_virtual_warning_feature = feature(
        name = "overloaded_virtual_warning",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Woverloaded-virtual"])],
            ),
        ],
    )

    # Warn when hiding a virtual function.
    no_overloaded_virtual_warning_feature = feature(
        name = "no_overloaded_virtual_warning",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-overloaded-virtual"])],
            ),
        ],
    )

    # Warn about moves of prvalues, which can inhibit copy elision.
    pessimizing_move_warning_feature = feature(
        name = "pessimizing_move_warning",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wpessimizing-move"])],
            ),
        ],
    )

    # -Wno-class-memaccess is only valid for C++ but not for C
    no_class_memaccess_warning_feature = feature(
        name = "no_class_memaccess_warning",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-class-memaccess"])],
            ),
        ],
    )

    # We shouldn't define any external-facing ABIs dependent on hardware interference sizes, so
    # inconsistent interference sizes between builds should not affect correctness.
    no_interference_size_warning_feature = feature(
        name = "no_interference_size_warning",
        enabled = ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = ["-Wno-interference-size"])],
            ),
        ],
    )

    thread_safety_warnings_feature = feature(
        name = "thread_safety_warnings",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = [
                    # Warn about thread safety issues
                    "-Wthread-safety",
                ])],
            ),
        ],
    )

    # Disable floating-point contractions such as forming of fused multiply-add
    # operations.
    disable_floating_point_contractions_feature = feature(
        name = "disable_floating_point_contractions",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-ffp-contract=off",
                ])],
            ),
        ],
    )

    enable_all_warnings_feature = feature(
        name = "enable_all_warnings_cc",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # Enable all warnings by default.
                    "-Wall",
                ])],
            ),
        ],
    )

    general_clang_or_gcc_warnings_feature = feature(
        name = "general_clang_or_gcc_warnings",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # Warn on comparison between signed and unsigned integer expressions.
                    "-Wsign-compare",

                    # Warn if a precompiled header (see Precompiled Headers) is found in the
                    # search path but can't be used.
                    "-Winvalid-pch",

                    # This has been suppressed in gcc 4.8, due to false positives, but not
                    # in clang. So we explicitly disable it here.
                    "-Wno-missing-braces",

                    # SERVER-76472 we don't try to maintain ABI so disable warnings about
                    # possible ABI issues.
                    "-Wno-psabi",
                ])],
            ),
        ],
    )

    general_clang_warnings_feature = feature(
        name = "general_clang_warnings",
        enabled = ctx.attr.compiler == COMPILERS.CLANG,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # SERVER-44856: Our windows builds complain about unused
                    # exception parameters, but GCC and clang don't seem to do
                    # that for us automatically. In the interest of making it more
                    # likely to catch these errors early, add the (currently clang
                    # only) flag that turns it on.
                    "-Wunused-exception-parameter",

                    # As of clang-3.4, this warning appears in v8, and gets escalated to an
                    # error.
                    "-Wno-tautological-constant-out-of-range-compare",

                    # As of clang in Android NDK 17, these warnings appears in boost and/or
                    # ICU, and get escalated to errors
                    "-Wno-tautological-constant-compare",
                    "-Wno-tautological-unsigned-zero-compare",
                    "-Wno-tautological-unsigned-enum-zero-compare",

                    # Suppress warnings about not consistently using override everywhere in
                    # a class. It seems very pedantic, and we have a fair number of
                    # instances.
                    "-Wno-inconsistent-missing-override",

                    # This warning was added in clang-4.0, but it warns about code that is
                    # required on some platforms. Since the warning just states that
                    # 'explicit instantiation of [a template] that occurs after an explicit
                    # specialization has no effect', it is harmless on platforms where it
                    # isn't required
                    "-Wno-instantiation-after-specialization",
                ])],
            ),
        ],
    )

    general_gcc_warnings_feature = feature(
        name = "general_gcc_warnings",
        enabled = ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # Disable warning about variables that may not be initialized
                    # Failures are triggered in the case of boost::optional
                    "-Wno-maybe-uninitialized",

                    # Prevents warning about unused but set variables found in boost version
                    # 1.49 in boost/date_time/format_date_parser.hpp which does not work for
                    # compilers GCC >= 4.6. Error explained in
                    # https://svn.boost.org/trac/boost/ticket/6136 .
                    "-Wno-unused-but-set-variable",
                ])],
            ),
        ],
    )

    general_gcc_or_clang_options_feature = feature(
        name = "general_gcc_or_clang_options",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # Generate unwind table in DWARF format, if supported by target machine.
                    # The table is exact at each instruction boundary, so it can be used for
                    # stack unwinding from asynchronous events (such as debugger or garbage
                    # collector).
                    "-fasynchronous-unwind-tables",

                    # For debug builds with tcmalloc, we need the frame pointer so it can
                    # record the stack of allocations. We also need the stack pointer for
                    # stack traces unless libunwind is enabled. Enable frame pointers by
                    # default.
                    "-fno-omit-frame-pointer",

                    # Enable strong by default, this may need to be softened to
                    # -fstack-protector-all if we run into compatibility issues.
                    "-fstack-protector-strong",

                    # Disable TBAA optimization
                    "-fno-strict-aliasing",

                    # Show colors even though bazel captures stdout/stderr
                    "-fdiagnostics-color",
                ])],
            ),
        ],
    )

    clang_fno_limit_debug_info_feature = feature(
        name = "clang_fno_limit_debug_info",
        enabled = ctx.attr.compiler == COMPILERS.CLANG,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    # We add this flag to make clang emit debug info for c++ stl types so
                    # that our pretty printers will work with newer clang's which omit this
                    # debug info. This does increase the overall debug info size.
                    "-fno-limit-debug-info",
                ])],
            ),
        ],
    )

    general_linkflags_feature = feature(
        name = "general_linkflags",
        enabled = ctx.attr.compiler == COMPILERS.CLANG or ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    # Explicitly use the new gnu hash section if the linker offers it.
                    "-Wl,--hash-style=gnu",

                    # Disallow an executable stack. Also, issue a warning if any files are
                    # found that would cause the stack to become executable if the
                    # noexecstack flag was not in play, so that we can find them and fix
                    # them. We do this here after we check for ld.gold because the
                    # --warn-execstack is currently only offered with gold.
                    "-Wl,-z,noexecstack",
                    "-Wl,--warn-execstack",

                    # If possible with the current linker, mark relocations as read-only.
                    "-Wl,-z,relro",
                ])],
            ),
        ],
    )

    build_id_feature = feature(
        name = "build_id",
        enabled = ctx.attr.compiler == COMPILERS.CLANG or ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    # Explicitly enable GNU build id's if the linker supports it.
                    "-Wl,--build-id",
                ])],
            ),
        ],
    )

    global_libs_feature = feature(
        name = "global_libs",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    "-lm",
                    "-lresolv",
                    "-latomic",
                ])],
            ),
        ],
    )

    pthread_feature = feature(
        name = "pthread",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    # Adds support for multithreading with the pthreads library. This option
                    # sets flags for both the preprocessor and linker.
                    "-pthread",
                ])],
            ),
        ],
    )

    compress_debug_sections_feature = feature(
        name = "compress_debug_sections",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    "-Wl,--compress-debug-sections=none",
                ])],
            ),
        ],
    )

    # Define rdynamic for backtraces with glibc unless we have libunwind.
    rdynamic_feature = feature(
        name = "rdynamic",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = [
                    # Pass the flag -export-dynamic to the ELF linker, on targets that
                    # support it. This instructs the linker to add all symbols, not only
                    # used ones, to the dynamic symbol table. This option is needed for some
                    # uses of dlopen or to allow obtaining backtraces from within a program.
                    "-rdynamic",
                ])],
            ),
        ],
    )

    gcc_no_ignored_attributes_features = feature(
        name = "gcc_no_ignored_attributes",
        enabled = ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-Wno-ignored-attributes",
                ])],
            ),
        ],
    )

    clang_toolchain_resource_dir_feature = feature(
        name = "clang_toolchain_resource_dir",
        enabled = ctx.attr.compiler == COMPILERS.CLANG,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-resource-dir=" + CLANG_RESOURCE_DIR],
                    ),
                ],
            ),
        ],
    )

    # Some of the linux versions are missing libatomic.so.1 - this is a hack so mold will use the one contained
    # within the mongo toolchain rather than needing one installed on the machine
    mold_shared_libraries_feature = feature(
        name = "mold_shared_libraries",
        enabled = ctx.attr.linker == LINKERS.MOLD,
        env_sets = [
            env_set(
                actions = all_link_actions,
                env_entries = [env_entry(key = "LD_LIBRARY_PATH", value = "external/mongo_toolchain_v5/stow/gcc-v5/lib64/")],
            ),
        ],
    )

    pgo_profile_generate_feature = feature(
        name = "pgo_profile_generate",
        enabled = ctx.attr.pgo_profile_generate,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                ] + all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fprofile-generate",
                            "-fno-data-sections",
                        ] if ctx.attr.compiler == COMPILERS.CLANG else [
                            "-fprofile-generate",
                            "-fno-data-sections",
                            "-fprofile-dir=mongod_perf",
                            "-Wl,-S",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = [
                    ACTION_NAMES.cpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [""] if ctx.attr.compiler == COMPILERS.CLANG else ["-Wno-mismatched-new-delete"],
                    ),
                ],
            ),
        ],
    )

    pgo_profile_use_feature = feature(
        name = "pgo_profile_use",
        enabled = ctx.attr.pgo_profile_use != None,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fprofile-use=" + ctx.attr.pgo_profile_use[DefaultInfo].files.to_list()[0].path if ctx.attr.pgo_profile_use != None else "",
                            "-Wno-profile-instr-unprofiled",
                            "-Wno-profile-instr-out-of-date",
                            "-Wno-backend-plugin",
                            "-mllvm",
                            "-profile-accurate-for-symsinlist=false",
                        ] if ctx.attr.compiler == COMPILERS.CLANG else [
                            "-fprofile-use",
                            "-Wno-missing-profile",
                            "-fprofile-correction",
                            "-Wno-coverage-mismatch",
                            "-fprofile-dir=" + ctx.attr.pgo_profile_use[DefaultInfo].files.to_list()[0].dirname if ctx.attr.pgo_profile_use != None else "",
                        ],
                    ),
                ],
            ),
        ],
    )

    propeller_profile_generate_feature = feature(
        name = "propeller_profile_generate",
        enabled = ctx.attr.propeller_profile_generate,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                ] + all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-funique-internal-linkage-names",
                            "-fbasic-block-address-map",
                        ],
                    ),
                ],
            ),
        ],
    )

    propeller_profile_use_cc_feature = feature(
        name = "propeller_profile_use_cc",
        enabled = ctx.attr.propeller_profile_use != None,
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                ],
                flag_groups = [
                    flag_group(
                        flags = [
                            "-funique-internal-linkage-names",
                            "-fbasic-block-sections=list=CCprofile.txt",
                        ],
                    ),
                ],
            ),
        ],
    )

    propeller_profile_use_link_feature = feature(
        name = "propeller_profile_use_link",
        enabled = ctx.attr.propeller_profile_use != None,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Wl,--symbol-ordering-file=LINKERprofile.txt",
                        ],
                    ),
                ],
            ),
        ],
    )

    # This is from bazels toolchain. Modified to add our own implies/enabled section.
    thinlto_feature = feature(
        name = "thin_lto",
        enabled = ctx.attr.distributed_thin_lto,
        implies = ["no_debug_types_section", "supports_start_end_lib"],
        flag_sets = [
            flag_set(
                actions = [
                    ACTION_NAMES.c_compile,
                    ACTION_NAMES.cpp_compile,
                ] + all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(flags = ["-flto=thin"]),
                    flag_group(
                        expand_if_available = "lto_indexing_bitcode_file",
                        flags = [
                            "-Xclang",
                            "-fthin-link-bitcode=%{lto_indexing_bitcode_file}",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = [ACTION_NAMES.linkstamp_compile],
                flag_groups = [flag_group(flags = ["-DBUILD_LTO_TYPE=thin"])],
            ),
            flag_set(
                actions = lto_index_actions,
                flag_groups = [
                    flag_group(flags = [
                        "-flto=thin",
                        "-Wl,-plugin-opt,thinlto-index-only%{thinlto_optional_params_file}",
                        "-Wl,-plugin-opt,thinlto-emit-imports-files",
                        "-Wl,-plugin-opt,thinlto-prefix-replace=%{thinlto_prefix_replace}",
                    ]),
                    flag_group(
                        expand_if_available = "thinlto_object_suffix_replace",
                        flags = [
                            "-Wl,-plugin-opt,thinlto-object-suffix-replace=%{thinlto_object_suffix_replace}",
                        ],
                    ),
                    flag_group(
                        expand_if_available = "thinlto_merged_object_file",
                        flags = [
                            "-Wl,-plugin-opt,obj-path=%{thinlto_merged_object_file}",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = [ACTION_NAMES.lto_backend],
                flag_groups = [
                    flag_group(flags = [
                        "-c",
                        "-fthinlto-index=%{thinlto_index}",
                        "-o",
                        "%{thinlto_output_object_file}",
                        "-x",
                        "ir",
                        "%{thinlto_input_bitcode_file}",
                        "-Wno-unused-command-line-argument",
                    ]),
                ],
            ),
        ],
    )

    debug_types_section_feature = feature(
        name = "debug_types_section",
        enabled = (ctx.attr.compiler == COMPILERS.CLANG and ctx.attr.linkstatic) or (ctx.attr.compiler == COMPILERS.GCC and ctx.attr.linkstatic and ctx.attr.distro == "suse15"),
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["-fdebug-types-section"])],
            ),
        ],
    )

    no_debug_types_section_feature = feature(
        name = "no_debug_types_section",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["-fno-debug-types-section"])],
            ),
        ],
    )

    file_prefix_map = feature(
        name = "file_prefix_map",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions + all_link_actions + lto_index_actions,
                flag_groups = [flag_group(flags = ["-ffile-prefix-map=/proc/self/cwd=."])],
            ),
        ],
    )

    strip_debug_feature = feature(
        name = "strip_debug",
        enabled = False,
        flag_sets = [
            flag_set(
                actions = all_link_actions,
                flag_groups = [flag_group(flags = ["-Wl,--strip-debug"])],
            ),
        ],
    )

    shared_archive_gcc_feature = feature(
        name = "shared_archive_gcc",
        enabled = ctx.attr.shared_archive and ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["-fno-gnu-unique"])],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-Wl,--no-gnu-unique", "-Wl,-Bsymbolic"] if ctx.attr.linker != LINKERS.MOLD else ["-Wl,-Bsymbolic"],
                    ),
                ],
            ),
        ],
    )

    # SERVER-9761: Ensure early detection of missing symbols in dependent libraries
    # at program startup. For non-release dynamic builds we disable this behavior in
    # the interest of improved mongod startup times. Xcode15 removed bind_at_load
    # functionality so we cannot have a selection for macosx here
    # ld: warning: -bind_at_load is deprecated on macOS
    # TODO: SERVER-90596 reenable loading at startup
    bind_at_load_feature = feature(
        name = "bind_at_load",
        enabled = ctx.attr.linkstatic,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [flag_group(flags = ["-Wl,-z,now"])],
            ),
        ],
    )

    implicit_fallthrough_feature = feature(
        name = "implicit_fallthrough",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-Wimplicit-fallthrough"] if ctx.attr.compiler == COMPILERS.CLANG else ["-Wimplicit-fallthrough=5"],
                    ),
                ],
            ),
        ],
    )

    any_sanitizer_feature = feature(
        name = "any_sanitizer",
        enabled = ctx.attr.any_sanitizer_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = ["-fno-omit-frame-pointer"])],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    # Sanitizer libs may inject undefined refs (for hooks) at link time, but the
                    # symbols will be available at runtime via the compiler runtime lib.
                    flag_group(
                        flags = [
                            "-Wl,--allow-shlib-undefined",
                            "-rtlib=compiler-rt",
                            "-unwindlib=libgcc",
                        ] if not ctx.attr.tsan_enabled else ["-Wl,--allow-shlib-undefined"],
                    ),
                ],
            ),
        ],
    )

    asan_feature = feature(
        name = "asan",
        enabled = ctx.attr.asan_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=address",
                            "-fsanitize-blacklist=" + ctx.attr.asan_denylist[DefaultInfo].files.to_list()[0].path if ctx.attr.asan_denylist != None else "",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = ["-fsanitize=address"],
                    ),
                ],
            ),
        ],
    )

    # We can't include the fuzzer flag with the other sanitize flags. The libfuzzer
    # library already has a main function, which will cause the dependencies check
    # to fail
    fsan_feature = feature(
        name = "fsan",
        enabled = ctx.attr.fsan_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=fuzzer-no-link",
                            "-fprofile-instr-generate",
                            "-fcoverage-mapping",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=fuzzer-no-link",
                            "-fprofile-instr-generate",
                            "-fcoverage-mapping",
                            "-nostdlib++",
                            "-lstdc++",
                        ],
                    ),
                ],
            ),
        ],
    )

    # Makes it easier to debug memory failures at the cost of some perf:
    #   -fsanitize-memory-track-origins
    msan_feature = feature(
        name = "msan",
        enabled = ctx.attr.msan_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=memory",
                            "-fsanitize-memory-track-origins",
                            "-fsanitize-blacklist=" + ctx.attr.msan_denylist[DefaultInfo].files.to_list()[0].path if ctx.attr.msan_denylist != None else "",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=memory",
                        ],
                    ),
                ],
            ),
        ],
    )

    tsan_feature = feature(
        name = "tsan",
        enabled = ctx.attr.tsan_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=thread",
                            "-fsanitize-blacklist=" + ctx.attr.tsan_denylist[DefaultInfo].files.to_list()[0].path if ctx.attr.tsan_denylist != None else "",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=thread",
                        ],
                    ),
                ],
            ),
        ],
    )

    # By default, undefined behavior sanitizer doesn't stop on the first error. Make
    # it so. Newer versions of clang have renamed the flag. However, this flag
    # cannot be included when using the fuzzer sanitizer if we want to suppress
    # errors to uncover new ones.

    # In dynamic builds, the `vptr` sanitizer check can require additional
    # dependency edges. That is very inconvenient, because such builds can't use
    # z,defs. The result is a very fragile link graph, where refactoring the link
    # graph in one place can have surprising effects in others. Instead, we just
    # disable the `vptr` sanitizer for dynamic builds. We tried some other
    # approaches in SERVER-49798 of adding a new descriptor type, but that didn't
    # address the fundamental issue that the correct link graph for a dynamic+ubsan
    # build isn't the same as the correct link graph for a regular dynamic build.
    ubsan_compile_flags = []
    if not ctx.attr.fsan_enabled:
        ubsan_compile_flags += ["-fno-sanitize-recover"]
    if not ctx.attr.linkstatic:
        ubsan_compile_flags += ["-fno-sanitize=vptr"]
    ubsan_feature = feature(
        name = "ubsan",
        enabled = ctx.attr.ubsan_enabled,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=undefined",
                            "-fsanitize-blacklist=" + ctx.attr.ubsan_denylist[DefaultInfo].files.to_list()[0].path if ctx.attr.ubsan_denylist != None else "",
                        ] + ubsan_compile_flags,
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fsanitize=undefined",
                        ] + ([] if ctx.attr.linkstatic else ["-fno-sanitize=vptr"]),
                    ),
                ],
            ),
        ],
    )

    # Suppress the function sanitizer check for third party libraries, because:
    #
    # - mongod (a C++ binary) links in WiredTiger (a C library)
    # - If/when mongod--built under ubsan--fails, the sanitizer will by
    #   default analyze the failed execution for undefined behavior related to
    #   function pointer usage. See:
    #   https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#available-checks
    # - When this happens, the sanitizer will attempt to dynamically load to perform
    #   the analysis.
    # - However, since WT was built as a C library, is not linked with the function
    #   sanitizer library symbols despite its C++ dependencies referencing them.
    # - This will cause the sanitizer itself to fail, resulting in debug information
    #   being unavailable.
    # - So by suppressing the function ubsan check, we won't reference symbols
    #   defined in the unavailable ubsan function sanitier library and will get
    #   useful debugging information.
    ubsan_third_party_feature = feature(
        name = "ubsan_third_party",
        enabled = ctx.attr.ubsan_enabled and not ctx.attr.linkstatic,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fno-sanitize=function",
                        ],
                    ),
                ],
            ),
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-fno-sanitize=function",
                        ],
                    ),
                ],
            ),
        ],
    )

    mtune_flags = []
    if ctx.attr.is_aarch64:
        mtune_flags += ["-march=armv8.2-a", "-mtune=generic"]
    if ctx.attr.is_ppc64le:
        mtune_flags += ["-mcpu=power8", "-mtune=power8", "-mcmodel=medium"]
    if ctx.attr.is_s390x:
        mtune_flags += ["-march=z196", "-mtune=zEC12"]
    if ctx.attr.is_x86_64:
        mtune_flags += ["-march=sandybridge", "-mtune=generic", "-mprefer-vector-width=128"]

    mtune_march_feature = feature(
        name = "mtune_march",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        flags = mtune_flags,
                    ),
                ],
            ),
        ],
    )

    compress_debug_disable_feature = feature(
        name = "compress_debug_disable",
        enabled = not ctx.attr.compress_debug_enabled and ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-Wa,--nocompress-debug-sections",
                ])],
            ),
        ],
    )

    rpath_override_feature = feature(
        name = "rpath_override",
        enabled = not ctx.attr.linkstatic,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-Wl,-z,origin",
                            "-Wl,--enable-new-dtags",
                            "-Wl,-rpath,\\$ORIGIN/../lib",
                        ],
                    ),
                ],
            ),
        ],
    )

    warnings_as_errors_link_feature = feature(
        name = "warnings_as_errors_link",
        enabled = ctx.attr.warnings_as_errors_enabled,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [flag_group(flags = [
                    "-Wl,--fatal-warnings",
                ])],
            ),
        ],
    )

    disable_warnings_for_third_party_libraries_clang_feature = feature(
        name = "disable_warnings_for_third_party_libraries_clang",
        enabled = ctx.attr.compiler == COMPILERS.CLANG,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-Wno-deprecated-declarations",
                    "-Wno-deprecated-non-prototype",
                    "-Wno-missing-template-arg-list-after-template-kw",
                    "-Wno-sign-compare",
                    "-Wno-implicit-fallthrough",
                ])],
            ),
        ],
    )

    disable_warnings_for_third_party_libraries_gcc_feature = feature(
        name = "disable_warnings_for_third_party_libraries_gcc",
        enabled = ctx.attr.compiler == COMPILERS.GCC,
        flag_sets = [
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-Wno-overloaded-virtual",
                    "-Wno-dangling-reference",
                    "-Wno-deprecated",
                    "-Wno-deprecated-declarations",
                    "-Wno-class-memaccess",
                    "-Wno-uninitialized",
                    "-Wno-array-bounds",
                    "-Wno-sign-compare",
                    "-Wno-stringop-overflow",
                    "-Wno-stringop-overread",
                    "-Wno-restrict",
                    "-Wno-dangling-pointer",
                    "-Wno-implicit-fallthrough",
                ])],
            ),
            flag_set(
                actions = all_compile_actions,
                flag_groups = [flag_group(flags = [
                    "-Wno-attributes",
                    "-Wno-implicit-fallthrough",
                ])],
            ),
        ],
    )

    features = [
        enable_all_warnings_feature,
        general_clang_or_gcc_warnings_feature,
        bin_dirs_feature,
        default_compile_flags_feature,
        include_paths_feature,
        library_search_directories_feature,
        supports_dynamic_linker_feature,
        supports_start_end_lib_feature,
        supports_pic_feature,
        pic_feature,
        pie_feature,
        per_object_debug_info_feature,
        preprocessor_defines_feature,
        objcopy_embed_flags_feature,
        opt_feature,
        dbg_feature,
        sysroot_feature,
        unfiltered_compile_flags_feature,
        omitted_timestamps_feature,
        thin_archive_feature,
        includes_feature,
        dependency_file_feature,
        verbose_feature,
        dwarf4_feature,
        dwarf5_feature,
        dwarf32_feature,
        dwarf64_feature,
        # Debug level should be passed after dwarf
        # as many dwarf flags will just set g2
        dbg_level_0_feature,
        dbg_level_1_feature,
        dbg_level_2_feature,
        dbg_level_3_feature,
        disable_debug_symbols_feature,
        no_warn_non_virtual_destructor_feature,
        no_deprecated_enum_enum_conversion_feature,
        no_volatile_feature,
        fsized_deallocation_feature,
        overloaded_virtual_warning_feature,
        no_overloaded_virtual_warning_feature,
        pessimizing_move_warning_feature,
        no_class_memaccess_warning_feature,
        no_interference_size_warning_feature,
        thread_safety_warnings_feature,
        disable_floating_point_contractions_feature,
        general_clang_warnings_feature,
        general_gcc_warnings_feature,
        general_gcc_or_clang_options_feature,
        clang_fno_limit_debug_info_feature,
        general_linkflags_feature,
        pthread_feature,
        compress_debug_sections_feature,
        rdynamic_feature,
        global_libs_feature,
        build_id_feature,
        gcc_no_ignored_attributes_features,
        mold_shared_libraries_feature,
        pgo_profile_generate_feature,
        pgo_profile_use_feature,
        propeller_profile_generate_feature,
        propeller_profile_use_cc_feature,
        propeller_profile_use_link_feature,
        thinlto_feature,
        debug_types_section_feature,
        no_debug_types_section_feature,
        file_prefix_map,
        strip_debug_feature,
        clang_toolchain_resource_dir_feature,
        shared_archive_gcc_feature,
        bind_at_load_feature,
        implicit_fallthrough_feature,
        any_sanitizer_feature,
        asan_feature,
        fsan_feature,
        msan_feature,
        tsan_feature,
        ubsan_feature,
        ubsan_third_party_feature,
        mtune_march_feature,
        compress_debug_disable_feature,
        rpath_override_feature,
        warnings_as_errors_link_feature,
    ] + get_common_features(ctx) + [
        # These flags are at the bottom so they get applied after anything else.
        # These are things like the flags people apply directly on cc_library through copts/linkopts
        user_compile_flags_feature,
        extra_cflags_feature,
        extra_cxxflags_feature,
        extra_ldflags_feature,
        disable_warnings_for_third_party_libraries_clang_feature,
        disable_warnings_for_third_party_libraries_gcc_feature,
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

mongo_linux_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "builtin_sysroot": attr.string(),
        "cxx_builtin_include_directories": attr.string_list(mandatory = True),
        "cpu": attr.string(mandatory = True),
        "compiler": attr.string(mandatory = True),
        "linker": attr.string(mandatory = True),
        "distro": attr.string(mandatory = False),
        "extra_cflags": attr.string_list(mandatory = False),
        "extra_cxxflags": attr.string_list(mandatory = False),
        "extra_ldflags": attr.string_list(mandatory = False),
        "includes": attr.string_list(mandatory = False),
        "bin_dirs": attr.string_list(mandatory = False),
        "tool_paths": attr.string_dict(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "verbose": attr.bool(mandatory = False),
        "linkstatic": attr.bool(mandatory = True),
        "shared_archive": attr.bool(mandatory = True),
        "dwarf_version": attr.int(mandatory = False),
        "fission": attr.bool(mandatory = False),
        "debug_level": attr.int(mandatory = False),
        "disable_debug_symbols": attr.bool(mandatory = False),
        "optimization_level": attr.string(mandatory = False),
        "pgo_profile_generate": attr.bool(default = False, mandatory = False),
        "pgo_profile_use": attr.label(default = None, mandatory = False),
        "bolt_enabled": attr.bool(default = False, mandatory = False),
        "propeller_profile_generate": attr.bool(default = False, mandatory = False),
        "propeller_profile_use": attr.label(default = None, allow_single_file = True, mandatory = False),
        "distributed_thin_lto": attr.bool(default = False, mandatory = False),
        "any_sanitizer_enabled": attr.bool(default = False, mandatory = False),
        "asan_enabled": attr.bool(default = False, mandatory = False),
        "asan_denylist": attr.label(default = None, mandatory = False),
        "fsan_enabled": attr.bool(default = False, mandatory = False),
        "msan_enabled": attr.bool(default = False, mandatory = False),
        "msan_denylist": attr.label(default = None, mandatory = False),
        "tsan_enabled": attr.bool(default = False, mandatory = False),
        "tsan_denylist": attr.label(default = None, mandatory = False),
        "ubsan_enabled": attr.bool(default = False, mandatory = False),
        "ubsan_denylist": attr.label(default = None, mandatory = False),
        "is_aarch64": attr.bool(default = False, mandatory = False),
        "is_ppc64le": attr.bool(default = False, mandatory = False),
        "is_s390x": attr.bool(default = False, mandatory = False),
        "is_x86_64": attr.bool(default = False, mandatory = False),
        "internal_thin_lto_enabled": attr.bool(default = False, mandatory = False),
        "coverage_enabled": attr.bool(default = False, mandatory = False),
        "compress_debug_enabled": attr.bool(default = False, mandatory = False),
        "warnings_as_errors_enabled": attr.bool(default = False, mandatory = False),
    },
    provides = [CcToolchainConfigInfo],
)
