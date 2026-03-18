"""Cross-compilation toolchain configuration for building macOS binaries on Linux.

This toolchain is hermetic: all paths reference files within the generated external
repository (symlinked from LLVM and SDK), using execroot-relative paths so that both
local sandboxed builds and remote execution work correctly.
"""

load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "artifact_name_pattern",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
    "variable_with_value",
)
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

_OBJCPP_EXECUTABLE_ACTION_NAME = "objc++-executable"

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

# C-only compile actions (for C-specific workarounds)
all_c_compile_actions = [
    ACTION_NAMES.c_compile,
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

def _get_target_triple(cpu):
    """Returns the target triple for the given CPU architecture."""
    if cpu == "arm64":
        return "arm64-apple-macos"
    elif cpu == "x86_64":
        return "x86_64-apple-macos"
    else:
        fail("Unsupported CPU: " + cpu)

def _sanitizer_feature(name = "", specific_compile_flags = [], specific_link_flags = []):
    return feature(
        name = name,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(flags = [
                        "-fno-omit-frame-pointer",
                        "-fno-sanitize-recover=all",
                    ] + specific_compile_flags),
                ],
            ),
            flag_set(
                actions = all_link_actions,
                flag_groups = [
                    flag_group(flags = specific_link_flags),
                ],
            ),
        ],
    )

def _impl(ctx):
    tool_paths = [
        tool_path(name = name, path = path)
        for name, path in ctx.attr.tool_paths.items()
    ]
    action_configs = []

    # Get target triple for cross-compilation
    target_triple = _get_target_triple(ctx.attr.cpu)

    # All paths are relative to the execroot. The external repo's files live at
    # external/<repo_name>/ in the execroot. This prefix is computed by the
    # repository rule and passed as an attribute.
    prefix = ctx.attr.execroot_prefix

    # Paths to key directories within the repo (execroot-relative)
    libcxx_include = prefix + "/include/c++/v1"
    clang_builtin_include = prefix + "/include/clang"
    sdk_include = prefix + "/sysroot/usr/include"
    sdk_lib = prefix + "/sysroot/usr/lib"
    sdk_frameworks = prefix + "/sysroot/System/Library/Frameworks"

    # Build include directories (for cxx_builtin_include_directories)
    # These tell Bazel which directories the compiler considers "built-in" so it
    # doesn't require explicit deps for headers found there.
    sdk_include_dirs = [
        prefix,
        prefix + "/include/config_override",
        libcxx_include,
        clang_builtin_include,
        # Bundled ICU headers must be listed before SDK to avoid namespace mismatch
        "src/third_party/icu4c-57.1/source/common",
        "src/third_party/icu4c-57.1/source/i18n",
        sdk_include,
        sdk_frameworks + "/CoreFoundation.framework/Headers",
        sdk_frameworks + "/Security.framework/Headers",
        sdk_frameworks + "/IOKit.framework/Headers",
        sdk_frameworks + "/SystemConfiguration.framework/Headers",
        sdk_frameworks,
    ]

    # Sysroot path (execroot-relative)
    sysroot = prefix + "/sysroot"

    # Cross-compilation specific compile flags (shared by C and C++)
    # Note: -stdlib=libc++ is C++ only (causes -Werror,-Wunused-command-line-argument in C)
    # but -nostdinc++ and the include paths must be here to maintain correct include order
    # (libc++ headers before clang builtins before SDK headers).
    cross_compile_flags = [
        "--target=" + target_triple + ctx.attr.min_macos_version,
        "-isysroot",
        sysroot,
        "-mmacosx-version-min=" + ctx.attr.min_macos_version,
        "-fPIC",
        # Prevent clang from using SDK's libc++ headers; use LLVM's instead
        # (LLVM's libc++ has full C++20 support including std::atomic_ref)
        "-nostdinc++",
        # Override directory with modified __config_site (enables vendor availability
        # annotations so newer libc++ symbols like __hash_memory are inlined)
        "-isystem",
        prefix + "/include/config_override",
        # LLVM's libc++ headers (for C++ standard library with full C++20 support)
        # These contain wrapper headers like stddef.h that use #include_next
        "-isystem",
        libcxx_include,
        # Clang builtin headers SECOND - defines size_t, ptrdiff_t in global namespace
        # libc++ wrappers' #include_next will find these
        "-isystem",
        clang_builtin_include,
        # Bundled ICU 57.1 headers BEFORE SDK headers. The macOS SDK ships its own
        # ICU headers at usr/include/unicode/ which use a different namespace (plain
        # "icu" vs bundled "icu_57") and unversioned C API symbols. If SDK headers
        # are found first, the linker can't resolve them against the bundled library.
        "-isystem",
        "src/third_party/icu4c-57.1/source/common",
        "-isystem",
        "src/third_party/icu4c-57.1/source/i18n",
        # SDK's C headers LAST (needed for system library)
        "-isystem",
        sdk_include,
    ]

    # C++ specific compile flags - force include cstdlib for malloc/free
    # (needed because fmt sets _LIBCPP_REMOVE_TRANSITIVE_INCLUDES which
    # prevents transitive includes from libc++ headers)
    cpp_force_includes = ["-include", "cstdlib"]

    # Cross-compilation specific link flags
    cross_link_flags = [
        "--target=" + target_triple + ctx.attr.min_macos_version,
        "-isysroot",
        sysroot,
        "-mmacosx-version-min=" + ctx.attr.min_macos_version,
        "-L" + sdk_lib,
        "-F" + sdk_frameworks,
        "-lc++",
        # Use lld linker with macOS flavor.  -B adds the tools dir to clang's
        # program search path so it finds ld64.lld there (clang resolves its
        # own symlink and otherwise only searches next to the real binary).
        "-B" + prefix + "/tools",
        "-fuse-ld=lld",
        "-Wl,-platform_version,macos," + ctx.attr.min_macos_version + ",14.0",
        # Force-load all members of static libraries. This is required because
        # MongoDB uses MONGO_INITIALIZER registrations via static constructors
        # that have no direct symbol references - without this, ld64 drops the
        # object files containing them. The GNU ld equivalent is --whole-archive
        # but ld64 mode doesn't support positional wrapping; -all_load is global.
        "-Wl,-all_load",
    ]

    # Feature definitions
    supports_pic_feature = feature(
        name = "supports_pic",
        enabled = True,
    )

    supports_start_end_lib_feature = feature(
        name = "supports_start_end_lib",
        enabled = True,
    )

    gcc_quoting_for_param_files_feature = feature(
        name = "gcc_quoting_for_param_files",
        enabled = True,
    )

    static_link_cpp_runtimes_feature = feature(
        name = "static_link_cpp_runtimes",
        enabled = False,
    )

    # Default compile flags feature
    default_compile_flags_feature = feature(
        name = "default_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(flags = cross_compile_flags + [
                        "-Wall",
                        "-Wextra",
                        "-Werror=return-type",
                        "-fno-strict-aliasing",
                        "-fno-omit-frame-pointer",
                    ]),
                ],
            ),
            flag_set(
                actions = all_cpp_compile_actions,
                flag_groups = [
                    flag_group(flags = [
                        "-stdlib=libc++",
                        "-std=c++20",
                        # Fix ICU 57.1 header shadowing: when bundled ICU code includes
                        # "unicode/localpointer.h", the SDK's newer ICU headers may be
                        # found first. SDK headers use bare 'noexcept' while bundled ICU
                        # uses U_NOEXCEPT macro. Define it to ensure compatibility.
                        "-DU_NOEXCEPT=noexcept",
                        # Fix ICU 57.1 U_FINAL macro: when MongoDB source files (outside ICU)
                        # include ICU headers via angle brackets, the SDK's umachine.h may be
                        # found instead of the bundled one. Ensure U_FINAL expands to 'final'.
                        "-DU_FINAL=final",
                    ] + cpp_force_includes),
                ],
            ),
            # C-specific flags: Work around macOS SDK ICU headers using char16_t
            # which isn't available in C (macOS lacks the <uchar.h> C11 header).
            # The SDK's umachine.h unconditionally uses char16_t for internal ICU
            # builds (when U_COMMON_IMPLEMENTATION etc. are defined), so we must
            # define char16_t itself rather than just UCHAR_TYPE.
            flag_set(
                actions = all_c_compile_actions,
                flag_groups = [
                    flag_group(flags = [
                        "-Dchar16_t=uint16_t",
                    ]),
                ],
            ),
        ],
    )

    # Default link flags feature
    default_link_flags_feature = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(flags = cross_link_flags),
                ],
            ),
        ],
    )

    # User compile flags (from copts)
    user_compile_flags_feature = feature(
        name = "user_compile_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "user_compile_flags",
                        iterate_over = "user_compile_flags",
                        flags = ["%{user_compile_flags}"],
                    ),
                ],
            ),
        ],
    )

    # User link flags (from linkopts)
    user_link_flags_feature = feature(
        name = "user_link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "user_link_flags",
                        iterate_over = "user_link_flags",
                        flags = ["%{user_link_flags}"],
                    ),
                ],
            ),
        ],
    )

    # Include paths feature
    include_paths_feature = feature(
        name = "include_paths",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        iterate_over = "quote_include_paths",
                        expand_if_available = "quote_include_paths",
                        flags = ["-iquote", "%{quote_include_paths}"],
                    ),
                    flag_group(
                        iterate_over = "include_paths",
                        expand_if_available = "include_paths",
                        flags = ["-I%{include_paths}"],
                    ),
                    flag_group(
                        iterate_over = "system_include_paths",
                        expand_if_available = "system_include_paths",
                        flags = ["-isystem", "%{system_include_paths}"],
                    ),
                ],
            ),
        ],
    )

    # External include paths feature - handles dependencies like abseil-cpp
    external_include_paths_feature = feature(
        name = "external_include_paths",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        iterate_over = "external_include_paths",
                        expand_if_available = "external_include_paths",
                        flags = ["-isystem", "%{external_include_paths}"],
                    ),
                ],
            ),
        ],
    )

    # Dependency file feature
    dependency_file_feature = feature(
        name = "dependency_file",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "dependency_file",
                        flags = ["-MD", "-MF", "%{dependency_file}"],
                    ),
                ],
            ),
        ],
    )

    # Debug feature
    dbg_feature = feature(
        name = "dbg",
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(flags = ["-g", "-O0"]),
                ],
            ),
        ],
        implies = ["common"],
    )

    # Opt feature
    opt_feature = feature(
        name = "opt",
        flag_sets = [
            flag_set(
                actions = all_compile_actions,
                flag_groups = [
                    flag_group(flags = ["-O2", "-DNDEBUG"]),
                ],
            ),
        ],
        implies = ["common"],
    )

    # Common feature
    common_feature = feature(
        name = "common",
        implies = [
            "default_compile_flags",
            "default_link_flags",
        ],
    )

    # Sysroot feature
    sysroot_feature = feature(
        name = "sysroot",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions + all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "sysroot",
                        flags = ["--sysroot=%{sysroot}"],
                    ),
                ],
            ),
        ],
    )

    # Framework paths feature for macOS
    framework_paths_feature = feature(
        name = "framework_paths",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_compile_actions + all_link_actions,
                flag_groups = [
                    flag_group(
                        flags = [
                            "-F" + sdk_frameworks,
                        ],
                    ),
                ],
            ),
        ],
    )

    # Sanitizer features
    asan_feature = _sanitizer_feature(
        name = "asan",
        specific_compile_flags = ["-fsanitize=address"],
        specific_link_flags = ["-fsanitize=address"],
    )

    tsan_feature = _sanitizer_feature(
        name = "tsan",
        specific_compile_flags = ["-fsanitize=thread"],
        specific_link_flags = ["-fsanitize=thread"],
    )

    ubsan_feature = _sanitizer_feature(
        name = "ubsan",
        specific_compile_flags = ["-fsanitize=undefined"],
        specific_link_flags = ["-fsanitize=undefined"],
    )

    # Archiver flags feature
    archiver_flags_feature = feature(
        name = "archiver_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = [ACTION_NAMES.cpp_link_static_library],
                flag_groups = [
                    flag_group(
                        expand_if_available = "output_execpath",
                        flags = ["rcsD", "%{output_execpath}"],
                    ),
                    flag_group(
                        expand_if_available = "libraries_to_link",
                        iterate_over = "libraries_to_link",
                        flag_groups = [
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file",
                                ),
                                flags = ["%{libraries_to_link.name}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file_group",
                                ),
                                iterate_over = "libraries_to_link.object_files",
                                flags = ["%{libraries_to_link.object_files}"],
                            ),
                        ],
                    ),
                ],
            ),
        ],
    )

    # Output execpath feature for linking
    output_execpath_flags_feature = feature(
        name = "output_execpath_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "output_execpath",
                        flags = ["-o", "%{output_execpath}"],
                    ),
                ],
            ),
        ],
    )

    # Library search directories feature
    library_search_directories_feature = feature(
        name = "library_search_directories",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "library_search_directories",
                        iterate_over = "library_search_directories",
                        flags = ["-L%{library_search_directories}"],
                    ),
                ],
            ),
        ],
    )

    # Libraries to link feature
    libraries_to_link_feature = feature(
        name = "libraries_to_link",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = all_link_actions + lto_index_actions,
                flag_groups = [
                    flag_group(
                        expand_if_available = "libraries_to_link",
                        iterate_over = "libraries_to_link",
                        flag_groups = [
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file_group",
                                ),
                                iterate_over = "libraries_to_link.object_files",
                                flags = ["%{libraries_to_link.object_files}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "object_file",
                                ),
                                flags = ["%{libraries_to_link.name}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "interface_library",
                                ),
                                flags = ["%{libraries_to_link.name}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "static_library",
                                ),
                                flags = ["%{libraries_to_link.name}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "dynamic_library",
                                ),
                                flags = ["-l%{libraries_to_link.name}"],
                            ),
                            flag_group(
                                expand_if_equal = variable_with_value(
                                    name = "libraries_to_link.type",
                                    value = "versioned_dynamic_library",
                                ),
                                flags = ["-l:%{libraries_to_link.name}"],
                            ),
                        ],
                    ),
                ],
            ),
        ],
    )

    # Collect all features
    features = [
        supports_pic_feature,
        gcc_quoting_for_param_files_feature,
        static_link_cpp_runtimes_feature,
        common_feature,
        default_compile_flags_feature,
        default_link_flags_feature,
        user_compile_flags_feature,
        user_link_flags_feature,
        include_paths_feature,
        external_include_paths_feature,
        dependency_file_feature,
        dbg_feature,
        opt_feature,
        sysroot_feature,
        framework_paths_feature,
        asan_feature,
        tsan_feature,
        ubsan_feature,
        archiver_flags_feature,
        output_execpath_flags_feature,
        library_search_directories_feature,
        libraries_to_link_feature,
        # Apply MONGO_GLOBAL_DEFINES (enterprise defines, sanitizer defines, etc.)
        # as -D flags. This is a subset of get_common_features() - we only need the
        # defines, not the warnings_as_errors or optimization features which would
        # conflict with the cross-compilation toolchain's own settings.
        feature(
            name = "mongo_defines",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [flag_group(
                        flags = ["-D" + define for define in ctx.attr.global_defines],
                    )],
                ),
            ],
        ),
    ] + ([supports_start_end_lib_feature] if ctx.attr.supports_start_end_lib else [])

    # Artifact name patterns for macOS
    artifact_name_patterns = [
        artifact_name_pattern(
            category_name = "executable",
            prefix = "",
            extension = "",
        ),
        artifact_name_pattern(
            category_name = "dynamic_library",
            prefix = "lib",
            extension = ".dylib",
        ),
        artifact_name_pattern(
            category_name = "static_library",
            prefix = "lib",
            extension = ".a",
        ),
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,
        action_configs = action_configs,
        artifact_name_patterns = artifact_name_patterns,
        cxx_builtin_include_directories = sdk_include_dirs,
        toolchain_identifier = ctx.attr.toolchain_identifier,
        host_system_name = "x86_64-unknown-linux-gnu",
        target_system_name = target_triple,
        target_cpu = ctx.attr.cpu,
        target_libc = ctx.attr.target_libc,
        compiler = ctx.attr.compiler,
        abi_version = ctx.attr.abi_version,
        abi_libc_version = ctx.attr.abi_libc_version,
        tool_paths = tool_paths,
        builtin_sysroot = sysroot,
    )

mongo_apple_cross_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {
        "abi_libc_version": attr.string(mandatory = True),
        "abi_version": attr.string(mandatory = True),
        "compiler": attr.string(mandatory = True),
        "cpu": attr.string(mandatory = True),
        "llvm_version": attr.string(mandatory = True),
        "min_macos_version": attr.string(mandatory = True),
        "execroot_prefix": attr.string(mandatory = True),
        "target_libc": attr.string(mandatory = True),
        "tool_paths": attr.string_dict(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "supports_start_end_lib": attr.bool(default = False),
        "optimization_level": attr.string(mandatory = False),
        "debug_level": attr.int(mandatory = False),
        "internal_thin_lto_enabled": attr.bool(default = False, mandatory = False),
        "coverage_enabled": attr.bool(default = False, mandatory = False),
        "compress_debug_enabled": attr.bool(default = False, mandatory = False),
        "warnings_as_errors_enabled": attr.bool(default = True, mandatory = False),
        "linkstatic": attr.bool(mandatory = True),
        "global_defines": attr.string_list(default = []),
    },
    provides = [CcToolchainConfigInfo],
)
