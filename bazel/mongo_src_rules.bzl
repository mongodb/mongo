"""Common mongo-specific bazel build rules intended to be used in individual
BUILD files in the "src/" subtree.
"""

load("//bazel/toolchains/cc:mongo_defines.bzl", "MONGO_GLOBAL_DEFINES")
load(
    "//bazel/toolchains/cc:mongo_errors.bzl",
    "REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE",
)
load(
    "//bazel/toolchains/cc:mongo_compiler_flags.bzl",
    "get_copts",
    "get_linkopts",
)
load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@com_github_grpc_grpc//bazel:generate_cc.bzl", "generate_cc")
load("@poetry//:dependencies.bzl", "dependency")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_shared_library")
load("@rules_proto//proto:defs.bzl", "proto_library")
load(
    "//bazel:separate_debug.bzl",
    "CC_SHARED_LIBRARY_SUFFIX",
    "SHARED_ARCHIVE_SUFFIX",
    "WITH_DEBUG_SUFFIX",
    "extract_debuginfo",
    "extract_debuginfo_binary",
    "extract_debuginfo_test",
)
load("@local_host_values//:local_host_values_set.bzl", "NUM_CPUS")
load("@evergreen_variables//:evergreen_variables.bzl", "UNSAFE_COMPILE_VARIANT", "UNSAFE_VERSION_ID")
load("//bazel/toolchains/cc/mongo_windows:mongo_windows_cc_toolchain_config.bzl", "MIN_VER_MAP")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//bazel/config:generate_config_header.bzl", "generate_config_header")
load("//bazel/auto_header:auto_header.bzl", "binary_srcs_with_all_headers", "build_selects_and_flat_files", "concat_selects", "dedupe_preserve_order", "maybe_all_headers", "maybe_compute_auto_headers", "strings_only")

# These will throw an error if the following condition is not met:
# (libunwind == on && os == linux) || libunwind == off || libunwind == auto
LIBUNWIND_DEPS = select({
    "//bazel/config:_libunwind_disabled_by_auto": [],
    "//bazel/config:_libunwind_off": [],
    "//bazel/config:libunwind_enabled": ["//src/third_party/unwind:unwind"],
}, no_match_error = REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE)

REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE = """
Error:
  linking mongo dynamically is not currently supported on Windows
"""

# This is a hack to work around the fact that the cc_library flag
# additional_compiler_inputs doesn't exist in cc_binary. Instead, we add the
# denylists to srcs as header files to make them visible to the compiler
# executable.
SANITIZER_DENYLIST_HEADERS = select({
    "//bazel/config:asan_enabled": ["//etc:asan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:msan_enabled": ["//etc:msan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:tsan_enabled": ["//etc:tsan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:ubsan_enabled": ["//etc:ubsan_denylist_h"],
    "//conditions:default": [],
})

ASAN_OPTIONS = [
    "detect_leaks=1",
    "check_initialization_order=true",
    "strict_init_order=true",
    "abort_on_error=1",
    "disable_coredump=0",
    "handle_abort=1",
    "strict_string_checks=true",
    "detect_invalid_pointer_pairs=1",
]

LSAN_OPTIONS = [
    "report_objects=1",
    "suppressions=$(location //etc:lsan.suppressions)",
]

MSAN_OPTIONS = []

UBSAN_OPTIONS = [
    "print_stacktrace=1",
]

TSAN_OPTIONS = [
    "abort_on_error=1",
    "disable_coredump=0",
    "handle_abort=1",
    "halt_on_error=1",
    "report_thread_leaks=0",
    "die_after_fork=0",
    "history_size=4",
    "suppressions=$(location //etc:tsan.suppressions)",
]

LLVM_SYMBOLIZER_OPTION = [
    "external_symbolizer_path=$(location //:llvm_symbolizer)",
]

ASAN_ENV = select({
    "//bazel/config:asan_enabled": {
        "ASAN_OPTIONS": ":".join(ASAN_OPTIONS),
        "LSAN_OPTIONS": ":".join(LSAN_OPTIONS),
    },
    "//bazel/config:asan_enabled_clang": {
        "ASAN_OPTIONS": ":".join(ASAN_OPTIONS + LLVM_SYMBOLIZER_OPTION),
        "LSAN_OPTIONS": ":".join(LSAN_OPTIONS + LLVM_SYMBOLIZER_OPTION),
    },
    "//conditions:default": {},
})

MSAN_ENV = select({
    "//bazel/config:msan_enabled": {
        "MSAN_OPTIONS": ":".join(MSAN_OPTIONS),
    },
    "//bazel/config:msan_enabled_clang": {
        "MSAN_OPTIONS": ":".join(MSAN_OPTIONS + LLVM_SYMBOLIZER_OPTION),
    },
    "//conditions:default": {},
})

TSAN_ENV = select({
    "//bazel/config:tsan_enabled": {
        "TSAN_OPTIONS": ":".join(TSAN_OPTIONS),
    },
    "//bazel/config:tsan_enabled_clang": {
        "TSAN_OPTIONS": ":".join(TSAN_OPTIONS + LLVM_SYMBOLIZER_OPTION),
    },
    "//conditions:default": {},
})

UBSAN_ENV = select({
    "//bazel/config:ubsan_enabled": {
        "UBSAN_OPTIONS": ":".join(UBSAN_OPTIONS),
    },
    "//bazel/config:ubsan_enabled_clang": {
        "UBSAN_OPTIONS": ":".join(UBSAN_OPTIONS + LLVM_SYMBOLIZER_OPTION),
    },
    "//conditions:default": {},
})

ASAN_DATA = select({
    "//bazel/config:asan_enabled": [
        "//etc:lsan.suppressions",
    ],
    "//conditions:default": [],
})

TSAN_DATA = select({
    "//bazel/config:tsan_enabled": [
        "//etc:tsan.suppressions",
    ],
    "//conditions:default": [],
})

ANY_SAN_DATA = select({
    "//bazel/config:any_sanitizer_clang": [
        "//:llvm_symbolizer",
        "//:llvm_symbolizer_libs",
    ],
    "//conditions:default": [],
})

SANITIZER_ENV = ASAN_ENV | MSAN_ENV | TSAN_ENV | UBSAN_ENV
SANITIZER_DATA = ASAN_DATA + TSAN_DATA + ANY_SAN_DATA

LINKSTATIC_ENABLED = select({
    "//bazel/config:linkdynamic_required_settings": False,
    "//bazel/config:linkstatic_enabled": True,
}, no_match_error = REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE)

SKIP_ARCHIVE_ENABLED = select({
    "//bazel/config:skip_archive_disabled": False,
    "//conditions:default": True,
})

SKIP_ARCHIVE_FEATURE = select({
    "@platforms//os:windows": [],
    "//conditions:default": ["supports_start_end_lib"],
})

SEPARATE_DEBUG_ENABLED = select({
    "//bazel/config:separate_debug_enabled": True,
    "//conditions:default": False,
})

PDB_GENERATION_ENABLED = select({
    "//bazel/config:debug_symbols_disabled": False,
    "//conditions:default": True,
})

TCMALLOC_ERROR_MESSAGE = """
Error:\n" +
    Build failed due to unsupported platform for current allocator selection:

    '--allocator=tcmalloc-google' is supported on linux with
        aarch64 or x86_64
    '--allocator=tcmalloc-gperf' is supported on windows or
        linux, but not macos
    '--allocator=system' can be used on any platform
"""

TCMALLOC_DEPS = select({
    "//bazel/config:system_allocator_enabled": [],
    "//bazel/config:tcmalloc_google_enabled": [
        "//src/third_party/tcmalloc:tcmalloc",
        "//src/third_party/tcmalloc:tcmalloc_internal_percpu_tcmalloc",
        "//src/third_party/tcmalloc:tcmalloc_internal_sysinfo",
    ],
    "//bazel/config:tcmalloc_gperf_enabled": [
        "//src/third_party/gperftools:tcmalloc_minimal",
    ],
}, no_match_error = TCMALLOC_ERROR_MESSAGE)

SYMBOL_ORDER_FILES = [
    "//buildscripts:symbols.orderfile",
    "//buildscripts:symbols-al2023.orderfile",
]

# These are warnings are disabled globally at the toolchain level to allow external repository compilation.
# Re-enable them for MongoDB source code.
RE_ENABLE_DISABLED_3RD_PARTY_WARNINGS_FEATURES = select({
    "//bazel/config:compiler_type_clang": [
        "-disable_warnings_for_third_party_libraries_clang",
        "thread_safety_warnings",
    ],
    "//bazel/config:compiler_type_gcc": [
        "-disable_warnings_for_third_party_libraries_gcc",
    ],
    "//conditions:default": [],
})

MONGO_GLOBAL_SRC_DEPS = [
    "//src/third_party/abseil-cpp:absl_base",
    "//src/third_party/boost:headers",
    "//src/third_party/croaring:croaring",
    "//src/third_party/fmt:fmt",
    "//src/third_party/libstemmer_c:stemmer",
    "//src/third_party/murmurhash3:murmurhash3",
    "//src/third_party/tomcrypt-1.18.2:tomcrypt",
    "//src/third_party/immer:headers",
    "//src/third_party/SafeInt:headers",
    "//src/third_party/sasl:windows_sasl",
    "//src/third_party/valgrind:headers",
    "//src/third_party/abseil-cpp:absl_local_repo_deps",
]

MONGO_GLOBAL_ADDITIONAL_LINKER_INPUTS = SYMBOL_ORDER_FILES

def hex32(val):
    """Returns zero-padded 8-character lowercase hex string of 32-bit hash."""
    v = val & 0xFFFFFFFF  # wrap to 32-bit unsigned
    s = "%x" % v
    return ("0" * (8 - len(s))) + s

def force_includes_hdr(package_name, name):
    if package_name.startswith("src/mongo"):
        return select({
            "@platforms//os:windows": [
                "//src/mongo/platform:basic.h",
                "//src/mongo/platform:windows_basic.h",
            ],
            "//conditions:default": ["//src/mongo/platform:basic.h"],
        })

    if name in ["scripting", "scripting_mozjs_test", "encrypted_dbclient"]:
        return select({
            "//bazel/config:linux_aarch64": ["//src/third_party/mozjs:platform/aarch64/linux/build/js-config.h"],
            "//bazel/config:linux_ppc64le": ["//src/third_party/mozjs:platform/ppc64le/linux/build/js-config.h"],
            "//bazel/config:linux_s390x": ["//src/third_party/mozjs:platform/s390x/linux/build/js-config.h"],
            "//bazel/config:linux_x86_64": ["//src/third_party/mozjs:platform/x86_64/linux/build/js-config.h"],
            "//bazel/config:macos_aarch64": ["//src/third_party/mozjs:platform/aarch64/macOS/build/js-config.h"],
            "//bazel/config:macos_x86_64": ["//src/third_party/mozjs:platform/x86_64/macOS/build/js-config.h"],
            "//bazel/config:windows_x86_64": ["//src/third_party/mozjs:/platform/x86_64/windows/build/js-config.h"],
        })

    return []

def tidy_config_filegroup():
    if native.existing_rule("clang_tidy_config") == None:
        native.filegroup(
            name = "clang_tidy_config",
            srcs = native.glob(
                [".clang-tidy"],
                allow_empty = True,
            ),
            visibility = ["//visibility:public"],
        )

def mongo_cc_library(
        name,
        srcs = [],
        hdrs = [],
        textual_hdrs = [],
        deps = [],
        cc_deps = [],
        private_hdrs = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        cxxopts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        mongo_api_name = None,
        target_compatible_with = [],
        skip_global_deps = [],
        non_transitive_dyn_linkopts = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        no_undefined_ref_DO_NOT_USE = True,
        linkshared = False,
        skip_windows_crt_flags = False,
        shared_lib_name = "",
        win_def_file = None,
        auto_header = True,
        srcs_select = None,
        **kwargs):
    """Wrapper around cc_library.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      hdrs: The headers files of the target library.
      textual_hdrs: Textual headers. Might be used to include cpp files without
        compiling them.
      deps: The targets the library depends on.
      cc_deps: Same as deps, but doesn't get added as shared library dep.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in. These are applied
        transitively to all targets that depend on this target.
      includes: Any directory which should be exported to dependents, will be
        prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel
        cc_test rule. This argument is currently not supported. The mongo build
        must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions added to the compile line when building
        any source in this target, but not to the compile line of targets that
        depend on this.
      skip_global_deps: Globally injected dependencies to skip adding as a
        dependency (options: "libunwind", "allocator").
      non_transitive_dyn_linkopts: Any extra link options to pass in when
        linking dynamically. Unlike linkopts these are not applied transitively
        to all targets depending on this target, and are only used when linking
        this target itself.
        See https://jira.mongodb.org/browse/SERVER-89047 for motivation.
      defines: macro definitions added to the compile line when building any
        source in this target, as well as the compile line of targets that
        depend on this.
      additional_linker_inputs: Any additional files that you may want to pass
        to the linker, for example, linker scripts.
      linkshared: force this library to be linked dynamically, regardless of
        configuration. This should only be used for shared archive support,
        aka. a shared library with all of its dependencies linked to it statically.
    """
    if "libunwind" not in skip_global_deps:
        deps += LIBUNWIND_DEPS

    # 0) Build real select(...) objects + a flat list of files (strings)
    _select_objs, _select_flat_files = build_selects_and_flat_files(
        srcs_select,
        lib_name = name,
        debug = False,
    )

    # 1) What cc_* should see: plain srcs, then fold in each select(...) via '+'
    _final_srcs_for_cc = concat_selects(srcs, _select_objs)

    pkg = native.package_name()
    is_mongo_src = pkg.startswith("src/mongo")
    in_third_party = "third_party" in pkg

    if is_mongo_src:
        if auto_header and not in_third_party:
            # Introspection list for auto-headers (strings only!)
            _all_concrete_srcs = dedupe_preserve_order(strings_only(srcs) + _select_flat_files)

            _ah = maybe_compute_auto_headers(_all_concrete_srcs)
            if _ah != None and _ah:
                # dedupe only the _ah strings if you want, NOT the selector expression
                srcs = _final_srcs_for_cc + dedupe_preserve_order(_ah)
            else:
                srcs = _final_srcs_for_cc + private_hdrs

        elif not in_third_party:
            srcs = binary_srcs_with_all_headers(name, _final_srcs_for_cc, private_hdrs)
        else:
            srcs = _final_srcs_for_cc + private_hdrs

        if name != "mongoca" and name != "cyrus_sasl_windows_test_plugin":
            deps += MONGO_GLOBAL_SRC_DEPS
        features = features + RE_ENABLE_DISABLED_3RD_PARTY_WARNINGS_FEATURES
    else:
        srcs = _final_srcs_for_cc + private_hdrs

    if "modules/enterprise" in native.package_name():
        target_compatible_with += select({
            "//bazel/config:build_enterprise_enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        })
    elif "modules/atlas" in native.package_name():
        target_compatible_with += select({
            "//bazel/config:build_atlas_enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        })

    if "third_party" in native.package_name():
        tags = tags + ["third_party"]

    copts = get_copts(name, native.package_name(), copts, skip_windows_crt_flags)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)
    linkopts = get_linkopts(native.package_name(), linkopts)

    if mongo_api_name:
        visibility_support_defines_list = ["MONGO_USE_VISIBILITY", "MONGO_API_" + mongo_api_name]
        visibility_support_shared_lib_flags_list = ["-fvisibility=hidden"]
    else:
        visibility_support_defines_list = ["MONGO_USE_VISIBILITY"]
        visibility_support_shared_lib_flags_list = []

    visibility_support_defines = select({
        "//bazel/config:visibility_support_enabled_dynamic_linking_setting": visibility_support_defines_list,
        "//conditions:default": [],
    })

    visibility_support_shared_flags = select({
        "//bazel/config:visibility_support_enabled_dynamic_linking_non_windows_setting": visibility_support_shared_lib_flags_list,
        "//conditions:default": [],
    })

    linux_rpath_flags = [
        "-Wl,-z,origin",
        "-Wl,--enable-new-dtags",
        "-Wl,-rpath,\\$ORIGIN/../lib",
        "-Wl,-h,lib" + name + ".so",
    ]
    macos_rpath_flags = [
        "-Wl,-rpath,\\$ORIGIN/../lib",
        "-Wl,-install_name,@rpath/lib" + name + ".dylib",
    ]

    rpath_flags = select({
        "//bazel/config:linux_aarch64": linux_rpath_flags,
        "//bazel/config:linux_ppc64le": linux_rpath_flags,
        "//bazel/config:linux_s390x": linux_rpath_flags,
        "//bazel/config:linux_x86_64": linux_rpath_flags,
        "//bazel/config:macos_aarch64": macos_rpath_flags,
        "//bazel/config:macos_x86_64": macos_rpath_flags,
        "//bazel/config:windows_x86_64": [],
    })

    if no_undefined_ref_DO_NOT_USE:
        undefined_ref_flag = select({
            "//bazel/config:sanitize_address_required_settings": [],
            "//bazel/config:sanitize_thread_required_settings": [],
            "@platforms//os:macos": [],
            "@platforms//os:windows": [],
            "//conditions:default": ["-Wl,-z,defs"],
        })
    else:
        undefined_ref_flag = []
        tags = tags + ["skip_symbol_check"]

    tidy_config_filegroup()

    # Did not want to expose alwayslink for cc_library as it ends up getting
    # modified in extract_debuginfo
    if "alwayslink" not in kwargs:
        kwargs["alwayslink"] = SKIP_ARCHIVE_ENABLED

    cc_library(
        name = name + WITH_DEBUG_SUFFIX,
        srcs = srcs + SANITIZER_DENYLIST_HEADERS,
        hdrs = hdrs + fincludes_hdr,
        deps = deps + cc_deps,
        textual_hdrs = textual_hdrs,
        visibility = visibility,
        testonly = testonly,
        copts = copts,
        cxxopts = cxxopts,
        data = data,
        tags = tags + ["mongo_library", "check_symbol_target"],
        linkopts = linkopts,
        linkstatic = select({
            "@platforms//os:windows": True,
            "//conditions:default": linkstatic,
        }),
        local_defines = MONGO_GLOBAL_DEFINES + local_defines,
        defines = defines,
        includes = includes,
        features = SKIP_ARCHIVE_FEATURE + features,
        target_compatible_with = target_compatible_with,
        additional_linker_inputs = additional_linker_inputs + MONGO_GLOBAL_ADDITIONAL_LINKER_INPUTS,
        exec_properties = exec_properties,
        **kwargs
    )

    # This is used to build our static shared objects such as mongo_crypt_v1.so
    shared_library = None
    if linkshared:
        if "allocator" not in skip_global_deps:
            deps += TCMALLOC_DEPS

        cc_shared_library(
            name = name + CC_SHARED_LIBRARY_SUFFIX + WITH_DEBUG_SUFFIX,
            deps = [name + WITH_DEBUG_SUFFIX],
            visibility = visibility,
            tags = tags + ["mongo_library"],
            user_link_flags = get_linkopts(native.package_name()) + undefined_ref_flag + non_transitive_dyn_linkopts + rpath_flags + visibility_support_shared_flags + select({
                "//bazel/config:simple_build_id_enabled": ["-Wl,--build-id=0x" +
                                                           hex32(hash(name)) +
                                                           hex32(hash(name)) +
                                                           hex32(hash(str(UNSAFE_VERSION_ID) + str(UNSAFE_COMPILE_VARIANT)))],
                "//conditions:default": [],
            }),
            target_compatible_with = target_compatible_with,
            shared_lib_name = shared_lib_name,
            features = select({
                "//bazel/config:windows_debug_symbols_enabled": ["generate_pdb_file"],
                "//conditions:default": [],
            }) + select({
                "//bazel/config:simple_build_id_enabled": ["-build_id"],
                "//conditions:default": [],
            }),
            additional_linker_inputs = additional_linker_inputs + MONGO_GLOBAL_ADDITIONAL_LINKER_INPUTS,
            exec_properties = exec_properties,
            win_def_file = win_def_file,
        )
        shared_library = name + CC_SHARED_LIBRARY_SUFFIX + WITH_DEBUG_SUFFIX

    extract_debuginfo(
        name = name,
        binary_with_debug = ":" + name + WITH_DEBUG_SUFFIX,
        type = "library",
        tags = tags + ["mongo_library"],
        enabled = SEPARATE_DEBUG_ENABLED,
        enable_pdb = PDB_GENERATION_ENABLED,
        cc_shared_library = shared_library,
        linkstatic = LINKSTATIC_ENABLED,
        skip_archive = SKIP_ARCHIVE_ENABLED,
        visibility = visibility,
        deps = deps + cc_deps,
        exec_properties = exec_properties,
    )

def _mongo_cc_binary_and_test(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        skip_global_deps = [],
        env = {},
        _program_type = "",
        skip_windows_crt_flags = False,
        auto_header = True,
        srcs_select = None,
        **kwargs):
    if linkstatic == True:
        fail("""Linking specific targets statically is not supported.
        The mongo build must link entirely statically or entirely dynamically.
        This can be configured via //config/bazel:linkstatic.""")

    # 0) Build real select(...) objects + gather a flat, introspectable list of files
    _select_objs, _select_flat_files = build_selects_and_flat_files(
        srcs_select,
        lib_name = name,
        debug = False,
    )

    # 1) What we actually pass to cc_binary as srcs: (plain srcs) + (real select(...)s)
    _final_srcs_for_cc = concat_selects(srcs, _select_objs)

    pkg = native.package_name()
    is_mongo_src = pkg.startswith("src/mongo")
    in_third_party = "third_party" in pkg

    if is_mongo_src:
        if auto_header and not in_third_party:
            # What we use for *introspection* (auto-header computation): a plain list of strings
            _all_concrete_srcs = dedupe_preserve_order((srcs or []) + _select_flat_files)

            # Use the concrete list so the tool can iterate even if _final_srcs_for_cc contains selects
            _ah = maybe_compute_auto_headers(_all_concrete_srcs)
            if _ah != None:
                # Add transitive auto-headers to *srcs* (binary path)
                srcs = _final_srcs_for_cc + dedupe_preserve_order(_ah)

            else:
                # Couldn’t compute auto-headers → fall back to adding private_hdrs to srcs
                srcs = _final_srcs_for_cc + private_hdrs

        elif not in_third_party:
            # all_headers mode (binary flavor): returns a srcs list
            srcs = binary_srcs_with_all_headers(name, _final_srcs_for_cc, private_hdrs)
        else:
            # third_party inside src/mongo: append private headers as sources
            srcs = _final_srcs_for_cc + private_hdrs

        deps += MONGO_GLOBAL_SRC_DEPS
        features = features + RE_ENABLE_DISABLED_3RD_PARTY_WARNINGS_FEATURES
    else:
        # Non-mongo pkgs: append private headers as sources
        srcs = _final_srcs_for_cc + private_hdrs

    if "modules/enterprise" in native.package_name():
        target_compatible_with += select({
            "//bazel/config:build_enterprise_enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        })

        if "modules/enterprise/src/fle" not in native.package_name():
            target_compatible_with += select({
                "//bazel/config:ssl_enabled": [],
                "//conditions:default": ["@platforms//:incompatible"],
            })

    copts = get_copts(name, native.package_name(), copts, skip_windows_crt_flags)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)
    linkopts = get_linkopts(native.package_name(), linkopts)

    all_deps = deps

    if "libunwind" not in skip_global_deps:
        all_deps += LIBUNWIND_DEPS

    if "allocator" not in skip_global_deps:
        all_deps += TCMALLOC_DEPS

    linux_rpath_flags = [
        "-Wl,-z,origin",
        "-Wl,--enable-new-dtags",
        "-Wl,-rpath,\\$$ORIGIN/../lib",
    ]
    macos_rpath_flags = ["-Wl,-rpath,\\$$ORIGIN/../lib"]

    rpath_flags = select({
        "//bazel/config:linux_aarch64": linux_rpath_flags,
        "//bazel/config:linux_ppc64le": linux_rpath_flags,
        "//bazel/config:linux_s390x": linux_rpath_flags,
        "//bazel/config:linux_x86_64": linux_rpath_flags,
        "//bazel/config:macos_aarch64": macos_rpath_flags,
        "//bazel/config:macos_x86_64": macos_rpath_flags,
        "//bazel/config:windows_x86_64": [],
    })

    exec_properties |= select({
        "//bazel/config:link_timeout_enabled": {
            "cpp_link.timeout": "600",
        },
        "//conditions:default": {},
    })

    # This is used as a tool in part of the shared archive build, so it needs to be marked
    # as compatible with a shared archive build.
    if name in ["grpc_cpp_plugin", "protobuf_compiler"]:
        features = features + ["-pie", "pic"]
    else:
        target_compatible_with += select({
            "//bazel/config:shared_archive_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    args = {
        "name": name + WITH_DEBUG_SUFFIX,
        "srcs": srcs + fincludes_hdr + SANITIZER_DENYLIST_HEADERS,
        "deps": all_deps,
        "visibility": visibility,
        "testonly": testonly,
        "copts": copts,
        "data": data + SANITIZER_DATA + select({
            "//bazel/platforms:use_mongo_toolchain": ["//:gdb"],
            "//conditions:default": [],
        }),
        "tags": tags,
        "linkopts": linkopts + rpath_flags + select({
            "//bazel/config:thin_lto_enabled": ["-Wl,--threads=" + str(NUM_CPUS)],
            "//conditions:default": [],
        }) + select({
            "//bazel/config:simple_build_id_enabled": ["-Wl,--build-id=0x" +
                                                       hex32(hash(name)) +
                                                       hex32(hash(name)) +
                                                       hex32(hash(str(UNSAFE_VERSION_ID) + str(UNSAFE_COMPILE_VARIANT)))],
            "//conditions:default": [],
        }),
        "linkstatic": LINKSTATIC_ENABLED,
        "local_defines": MONGO_GLOBAL_DEFINES + local_defines,
        "defines": defines,
        "includes": includes,
        "features": SKIP_ARCHIVE_FEATURE + ["-pic", "pie"] + features + select({
            "//bazel/config:windows_debug_symbols_enabled": ["generate_pdb_file"],
            "//conditions:default": [],
        }) + select({
            "//bazel/config:simple_build_id_enabled": ["-build_id"],
            "//conditions:default": [],
        }),
        "target_compatible_with": target_compatible_with,
        "additional_linker_inputs": additional_linker_inputs + MONGO_GLOBAL_ADDITIONAL_LINKER_INPUTS,
        "exec_properties": exec_properties | select({
            "//bazel/config:remote_link_enabled": {},
            "//bazel/config:dtlto_enabled": {},
            "@platforms//os:windows": {"cpp_link.coefficient": "1.0"},
            "//conditions:default": {"cpp_link.coefficient": "18.0"},
        }) | select({
            "//bazel/config:thin_lto_enabled": {"cpp_link.cpus": str(NUM_CPUS)},
            "//conditions:default": {},
        }) | select({
            "//bazel/config:remote_link_arm_linux_linkstatic": {"cpp_link.Pool": "arm_linker"},
            "//conditions:default": {},
        }),
        "env": env | SANITIZER_ENV,
    } | kwargs

    # we dont want the intermediate build targets to be picked up by tags
    # so we empty it out
    original_tags = list(args["tags"])
    args["tags"] = ["intermediate_debug"] + [tag + "_debug" for tag in original_tags]
    if _program_type == "binary":
        cc_binary(**args)
        extract_debuginfo_binary(
            name = name,
            binary_with_debug = ":" + name + WITH_DEBUG_SUFFIX,
            type = "program",
            tags = original_tags + ["final_target"],
            enabled = SEPARATE_DEBUG_ENABLED,
            enable_pdb = PDB_GENERATION_ENABLED,
            deps = all_deps,
            visibility = visibility,
            exec_properties = exec_properties,
        )
    else:
        native.cc_test(**args)
        extract_debuginfo_test(
            name = name,
            binary_with_debug = ":" + name + WITH_DEBUG_SUFFIX,
            type = "program",
            tags = original_tags + ["final_target"],
            enabled = SEPARATE_DEBUG_ENABLED,
            enable_pdb = PDB_GENERATION_ENABLED,
            deps = all_deps,
            visibility = visibility,
            exec_properties = exec_properties,
        )

        native.sh_test(
            name = name + "_ci_wrapper",
            srcs = [
                "//bazel:test_wrapper",
            ],
            tags = original_tags + ["wrapper_target"],
            args = [
                "$(location " + name + ")",
            ],
            data = args["data"] + [name],
            env = args["env"],
            target_compatible_with = target_compatible_with,
            visibility = visibility,
            exec_properties = exec_properties,
        )

def mongo_cc_binary(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        skip_global_deps = [],
        env = {},
        **kwargs):
    """Wrapper around cc_binary.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      deps: The targets the library depends on.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in.
      includes: Any directory which should be exported to dependents, will be
        prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel
        cc_test rule. This argument is currently not supported. The mongo build
        must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions passed to all source and header files.
      defines: macro definitions added to the compile line when building any
        source in this target, as well as the compile line of targets that
        depend on this.
      additional_linker_inputs: Any additional files that you may want to pass
        to the linker, for example, linker scripts.
      skip_global_deps: Globally injected dependencies to skip adding as a
        dependency (options: "libunwind", "allocator").
      env: environment variables to pass to the binary when running through
        bazel.
    """
    _mongo_cc_binary_and_test(
        name,
        srcs,
        deps,
        private_hdrs,
        testonly,
        visibility,
        data,
        tags + ["mongo_binary"],
        copts,
        linkopts,
        includes,
        linkstatic,
        local_defines,
        target_compatible_with,
        defines,
        additional_linker_inputs,
        features,
        exec_properties,
        skip_global_deps,
        env,
        _program_type = "binary",
        **kwargs
    )

def mongo_cc_test(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        skip_global_deps = [],
        env = {},
        minimum_test_resources = {},
        **kwargs):
    """Wrapper around cc_test.

    Args:
      name: The name of the test target.
      srcs: The source files to build.
      deps: The targets the library depends on.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in.
      includes: Any directory which should be exported to dependents, will be
        prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel
        cc_test rule. This argument is currently not supported. The mongo build
        must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions passed to all source and header files.
      defines: macro definitions added to the compile line when building any
        source in this target, as well as the compile line of targets that
        depend on this.
      additional_linker_inputs: Any additional files that you may want to pass
        to the linker, for example, linker scripts.
      skip_global_deps: Globally injected dependencies to skip adding as a
        dependency (options: "libunwind", "allocator").
      env: environment variables to pass to the binary when running through
        bazel.
      minimum_test_resources: a dict of key/value pairs defining execution
        requirements for the test. The only currently supported key is "cpu_cores".
    """
    minimum_core_count = 1
    if "cpu_cores" in minimum_test_resources:
        minimum_core_count = minimum_test_resources["cpu_cores"]

    if minimum_core_count == 2:
        exec_properties = exec_properties | select({
            "@platforms//cpu:x86_64": {
                "test.Pool": "large_mem_2core_x86_64",
            },
            "@platforms//cpu:aarch64": {
                "test.Pool": "large_memory_2core_arm64",
            },
            "//conditions:default": {},
        })
    elif minimum_core_count > 2:
        fail("minimum_test_resources[\"cpu_cores\"] > 2 is not supported")

    _mongo_cc_binary_and_test(
        name,
        srcs,
        deps,
        private_hdrs,
        True,
        visibility,
        data,
        tags,
        copts,
        linkopts,
        includes,
        linkstatic,
        local_defines,
        target_compatible_with,
        defines,
        additional_linker_inputs,
        features,
        exec_properties,
        skip_global_deps,
        env | {
            # This flag is already always set when running bazel test. Setting it
            # here ensures that it is also set when running bazel run. This avoids
            # test-setup.sh piping stdout through tee, so that the binary will be
            # directly connected to the terminal and able to detect color support.
            # We can remove this once we are on bazel 9 because it has removed the
            # logic that looks for this var always behaves as if it is set.
            "EXPERIMENTAL_SPLIT_XML_GENERATION": "1",
        } | select({
            "//bazel/config:dev_stacktrace_enabled": {
                # Forcing bazel's test environment setup script to run from the
                # unsandboxed directory allows access to split DWARF files and source
                # files.
                "RUNTEST_PRESERVE_CWD": "1",
            },
            "//conditions:default": {},
        }),
        _program_type = "test",
        **kwargs
    )

def mongo_cc_unit_test(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = ["//visibility:public"],
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        has_custom_mainline = False,
        **kwargs):
    mongo_cc_test(
        name = name,
        srcs = srcs,
        deps = deps + ([] if has_custom_mainline else ["//src/mongo/unittest:unittest_main"]),
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags + ["mongo_unittest"],
        copts = copts,
        linkopts = linkopts,
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        target_compatible_with = target_compatible_with,
        defines = defines,
        additional_linker_inputs = additional_linker_inputs,
        features = features,
        exec_properties = exec_properties,
        **kwargs
    )

IdlInfo = provider(
    fields = {
        "header_output": "header output of the idl",
        "idl_deps": "depset of idl files",
    },
)

def idl_generator_impl(ctx):
    base = ctx.attr.src.files.to_list()[0].basename.removesuffix(".idl")
    gen_source = ctx.actions.declare_file(base + "_gen.cpp")
    gen_header = ctx.actions.declare_file(base + "_gen.h")

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    dep_depsets = [dep[IdlInfo].idl_deps for dep in ctx.attr.deps]

    # Transitive headers from deps + explicit hdrs attr
    transitive_header_outputs = (
        [dep[IdlInfo].header_output for dep in ctx.attr.deps] +
        [hdr[DefaultInfo].files for hdr in ctx.attr.hdrs]
    )

    # collect deps from python modules and setup the corresponding
    # path so all modules can be found by the toolchain.
    python_path = []
    for py_dep in ctx.attr.py_deps:
        for path in py_dep[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(ctx.expand_make_variables(
                    "python_library_imports",
                    "$(BINDIR)/external/" + path,
                    ctx.var,
                ))

    py_depsets = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.py_deps]

    inputs = depset(transitive = [
        ctx.attr.src.files,
        ctx.attr.idlc.files,
        python.files,
    ] + dep_depsets + py_depsets)

    include_directives = ["--include", "src"]
    if "src/mongo/db/modules/enterprise/src" in ctx.attr.src.files.to_list()[0].path:
        include_directives += ["--include", "src/mongo/db/modules/enterprise/src"]

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [gen_source, gen_header],
        inputs = inputs,
        arguments = [
            "buildscripts/idl/idlc.py",
            "--base_dir",
            ctx.bin_dir.path + "/src",
            "--target_arch",
            ctx.var["TARGET_CPU"],
            "--header",
            gen_header.path,
            "--output",
            gen_source.path,
            ctx.attr.src.files.to_list()[0].path,
        ] + include_directives,
        mnemonic = "IdlcGenerator",
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
    )

    # Depsets we’ll publish
    header_ds = depset([gen_header], transitive = transitive_header_outputs)
    all_files = depset([gen_source, gen_header], transitive = transitive_header_outputs)

    return [
        # Keep DefaultInfo as-is so :*_gen works in cc_library(srcs/hdrs)
        DefaultInfo(files = all_files),

        # Your custom provider (unchanged)
        IdlInfo(
            idl_deps = depset(
                ctx.attr.src.files.to_list(),
                transitive = [dep[IdlInfo].idl_deps for dep in ctx.attr.deps],
            ),
            header_output = header_ds,
        ),

        # NEW: expose header-only view for wrappers in the shadow BUILD
        OutputGroupInfo(
            # Most consumers use one of these two names—publish both:
            hdrs = header_ds,
            header_files = header_ds,

            # Optional: a cpp-only view if you ever want it
            srcs = depset([gen_source]),
            cpps = depset([gen_source]),
            # (You can omit srcs/cpps if you don't need them.)
        ),
    ]

idl_generator_rule = rule(
    idl_generator_impl,
    attrs = {
        "deps": attr.label_list(
            doc = "Other idl files that need to be imported.",
            providers = [IdlInfo],
        ),
        "idlc": attr.label(
            doc = "The idlc generator to use.",
            default = "//buildscripts/idl:idlc",
        ),
        "py_deps": attr.label_list(
            doc = "Python modules that should be imported.",
            providers = [PyInfo],
            default = [
                dependency("pyyaml", group = "core"),
                dependency("pymongo", group = "core"),
            ],
        ),
        "src": attr.label(
            doc = "The idl file to generate cpp/h files from.",
            allow_single_file = True,
        ),
        "hdrs": attr.label_list(
            doc = "Dependent headers required by this IDL target",
            allow_files = True,
            default = [],
        ),
    },
    outputs = {
        # These create addressable file labels:
        "gen_hdr": "%{name}.h",
        "gen_src": "%{name}.cpp",
    },
    doc = "Generates header/source files from IDL files.",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    fragments = ["py"],
)

def idl_generator(name, tags = [], hdrs = [], deps = [], idl_self_dep = False, **kwargs):
    # Extra headers always pulled by IDL
    if not idl_self_dep:
        idl_deps = deps + ["//src/mongo/db/query:explain_verbosity_gen"]
    else:
        idl_deps = deps

    idl_generator_rule(
        name = name,
        tags = tags + ["gen_source"],
        hdrs = hdrs + ["//src/mongo:idl_headers"],
        deps = idl_deps,
        **kwargs
    )

def symlink_impl(ctx):
    ctx.actions.symlink(
        output = ctx.outputs.output,
        target_file = ctx.attr.input.files.to_list()[0],
    )

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

symlink_rule = rule(
    symlink_impl,
    attrs = {
        "input": attr.label(
            doc = "The File that the output symlink will point to.",
            allow_single_file = True,
        ),
        "output": attr.output(
            doc = "The output of this rule.",
        ),
    },
)

def symlink(name, tags = [], **kwargs):
    symlink_rule(
        name = name,
        tags = tags + ["gen_source"],
        **kwargs
    )

def strip_deps_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    linker_input = ctx.attr.input[CcInfo].linking_context.linker_inputs.to_list()[0]
    linking_context = cc_common.create_linking_context(linker_inputs = depset(direct = [linker_input], transitive = []))

    return [DefaultInfo(files = ctx.attr.input.files), CcInfo(
        compilation_context = ctx.attr.input[CcInfo].compilation_context,
        linking_context = linking_context,
    )]

strip_deps = rule(
    strip_deps_impl,
    attrs = {
        "input": attr.label(
            providers = [CcInfo],
        ),
        "linkstatic": attr.bool(),
    },
    provides = [CcInfo],
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
)

def dummy_file_impl(ctx):
    ctx.actions.write(
        output = ctx.outputs.output,
        content = "",
    )

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

dummy_file = rule(
    dummy_file_impl,
    attrs = {
        "output": attr.output(
            doc = "The output of this rule.",
        ),
    },
)

def mongo_proto_library(
        name,
        srcs,
        tags = [],
        **kwargs):
    features = kwargs.pop("features", [])
    proto_library(
        name = name,
        srcs = srcs,
        tags = tags + ["gen_source"],
        features = features,
        **kwargs
    )

def mongo_cc_proto_library(
        name,
        deps,
        tags = [],
        **kwargs):
    native.cc_proto_library(
        name = name + "_raw",
        deps = deps,
        **kwargs
    )
    strip_deps(
        name = name,
        input = name + "_raw",
        tags = tags + ["gen_source"],
    )

def mongo_cc_grpc_library(
        name,
        srcs,
        cc_proto,
        deps = [],
        grpc_only = True,
        proto_only = False,
        well_known_protos = False,
        generate_mocks = False,
        tags = [],
        no_undefined_ref_DO_NOT_USE = True,
        **kwargs):
    codegen_grpc_target = "_" + name + "_grpc_codegen"

    # TODO(SERVER-100148): Re-enable sandboxing on protobuf compilation
    # once we can rely on //external:grpc_cpp_plugin.
    #
    # TSAN is currently being applied to protoc which is failing to run
    # under Bazel's sandbox due to the system call to disable ASLR
    # failing.
    #
    # To workaround this issue, disable the sandbox only when compiling
    # protobufs, since we don't care about threading issues in the
    # proto compiler itself.
    generate_cc(
        name = codegen_grpc_target,
        srcs = srcs,
        plugin = "//src/third_party/grpc:grpc_cpp_plugin",
        well_known_protos = well_known_protos,
        generate_mocks = generate_mocks,
        tags = tags + ["gen_source"],
        disable_sandbox = select({
            "//bazel/config:tsan_enabled": True,
            "//conditions:default": False,
        }),
        **kwargs
    )

    # cc_proto_library tacks on unnecessary link-time dependencies to
    # @com_google_protobuf and @com_google_absl, forcefully remove them
    # to avoid intefering with thin targets link line generation.
    cc_proto_target = "_" + name + "_cc_proto_stripped_deps"
    strip_deps(
        name = cc_proto_target,
        input = cc_proto,
    )

    mongo_cc_library(
        name = name,
        srcs = [":" + codegen_grpc_target],
        hdrs = [":" + codegen_grpc_target],
        deps = deps +
               ["//src/third_party/grpc:grpc++_codegen_proto"],
        cc_deps = [":" + cc_proto_target],
        no_undefined_ref_DO_NOT_USE = no_undefined_ref_DO_NOT_USE,
        **kwargs
    )

def mongo_idl_library(
        name,
        src,
        idl_deps = [],
        idl_hdrs = [],
        deps = [],
        **kwargs):
    """
    Args:
      name: The name of the IDL library.
      src: The IDL src.
      idl_deps: The idl_generator deps.
      idl_hdrs: The idl_generator hdrs.
      deps: The mongo_cc_library deps.
    """

    idl_gen_name = name + "_gen"
    idl_generator(
        name = idl_gen_name,
        src = src,
        hdrs = idl_hdrs,
        deps = idl_deps,
    )

    mongo_cc_library(
        name = name,
        srcs = [idl_gen_name],
        deps = deps,
        auto_header = False,
        **kwargs
    )

def mongo_cc_benchmark(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        has_custom_mainline = False,
        **kwargs):
    mongo_cc_test(
        name = name,
        srcs = srcs,
        deps = deps + ([] if has_custom_mainline else ["//src/mongo/unittest:benchmark_main"]),
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags + ["mongo_benchmark"],
        copts = copts,
        linkopts = linkopts,
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        target_compatible_with = target_compatible_with,
        defines = defines,
        additional_linker_inputs = additional_linker_inputs,
        features = features,
        exec_properties = exec_properties,
        **kwargs
    )

def mongo_cc_integration_test(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        has_custom_mainline = False,
        **kwargs):
    mongo_cc_test(
        name = name,
        srcs = srcs,
        deps = deps + ([] if has_custom_mainline else ["//src/mongo/unittest:integration_test_main"]),
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags + ["mongo_integration_test"],
        copts = copts,
        linkopts = linkopts,
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        target_compatible_with = target_compatible_with,
        defines = defines,
        additional_linker_inputs = additional_linker_inputs,
        features = features,
        exec_properties = exec_properties,
        **kwargs
    )

def mongo_cc_fuzzer_test(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        has_custom_mainline = False,
        **kwargs):
    mongo_cc_test(
        name = name,
        srcs = srcs,
        deps = deps,
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags + ["mongo_fuzzer_test"],
        copts = copts,
        linkopts = linkopts + ["-fsanitize=fuzzer"],
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        target_compatible_with = target_compatible_with + select({
            "//bazel/config:fsan_enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        defines = defines,
        additional_linker_inputs = additional_linker_inputs,
        features = features,
        exec_properties = exec_properties,
        **kwargs
    )

# Note: For these extensions to load successfully in the server, they must be built with
# --allocator=system. Otherwise, the extensions will get a local instance of tcmalloc which
# fails to run properly because there isn't enough TLS space available for both the host and
# extension's tcmalloc. In transitions.bzl, we define a Bazel transition for managing the allocator
# and other extension-specific options.
def mongo_cc_extension_shared_library(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        **kwargs):
    mongo_cc_library(
        name = name,
        srcs = srcs,
        deps = deps + [
            "//src/mongo/db/extension/public:extensions_api_public",
            "//src/mongo/db/extension/sdk:sdk_cpp",
        ],
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags,
        copts = copts,
        linkopts = linkopts,
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        defines = defines,
        features = features,
        exec_properties = exec_properties,
        additional_linker_inputs = additional_linker_inputs + select({
            "@platforms//os:linux": [
                ":test_extensions.version_script.lds",
            ],
            "//conditions:default": [],
        }) + select({
            "@platforms//os:macos": [
                ":test_extensions.exported_symbols_list.lds",
            ],
            "//conditions:default": [],
        }),
        # linkshared produces a shared library as the output.
        # TODO SERVER-109255 Make sure the test extensions are statically linked, as we expect
        # all extensions to be.
        linkshared = True,
        non_transitive_dyn_linkopts = select({
            "@platforms//os:linux": [
                "-Wl,--version-script=$(location :test_extensions.version_script.lds)",
            ],
            "//conditions:default": [],
        }) + select({
            "@platforms//os:macos": [
                "-Wl,-exported_symbols_list,$(location :test_extensions.exported_symbols_list.lds)",
            ],
            "//conditions:default": [],
        }),
        skip_global_deps = [
            # This is a globally injected dependency. We don't want a special allocator linked
            # here. Instead, the allocator should be overriden at load time.
            "allocator",
            "libunwind",
        ],
        target_compatible_with = target_compatible_with + select({
            "//bazel/config:shared_archive_or_link_dynamic": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }) + select({
            "@platforms//os:linux": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

def _compute_win_defines(ver):
    if ver not in MIN_VER_MAP:
        fail("Unsupported windows_version_minimal=%r; supported: %r" %
             (ver, sorted(MIN_VER_MAP.keys())))
    m = MIN_VER_MAP[ver]
    return [
        "_WIN32_WINNT=%s" % m["win"],
        "BOOST_USE_WINAPI_VERSION=%s" % m["win"],
        "NTDDI_VERSION=%s" % m["ddi"],
    ]

def _uniq_dirs(files):
    d = {}
    for f in files:
        d[f.dirname] = True
    return sorted(d.keys())

def _rc_impl(ctx):
    # No-op on non-Windows
    if not ctx.target_platform_has_constraint(
        ctx.attr._os_windows[platform_common.ConstraintValueInfo],
    ):
        li = cc_common.create_linker_input(
            owner = ctx.label,
            user_link_flags = [],
            additional_inputs = depset(),  # and declare it as a link input
        )
        new_cc_shared_info = CcSharedLibraryInfo(
            dynamic_deps = depset(),
            exports = [],
            linker_input = li,
            link_once_static_libs = {},
        )

        return [DefaultInfo(files = depset([])), CcInfo(), new_cc_shared_info]

    new_cc_shared_info = CcSharedLibraryInfo(
        dynamic_deps = depset(),
        exports = [],
        linker_input = None,
        link_once_static_libs = {},
    )

    # On Windows we need an rc executable
    if ctx.executable.rc == None:
        fail("windows_rc: 'rc' tool not provided. Pass rc = '@mongo_windows_toolchain//:rc' (or your wrapper).")

    out = ctx.actions.declare_file(ctx.label.name + ".res")

    defines = []
    if ctx.attr.windows_version_minimal:
        ver = ctx.attr.windows_version_minimal[BuildSettingInfo].value
        defines += _compute_win_defines(ver)
    defines += ctx.attr.defines

    include_dirs = ["src", ctx.file.src.dirname]

    # Add $(GENDIR)/src (use backslashes for rc.exe friendliness)
    gen_src = ctx.var["GENDIR"] + "\\src"
    include_dirs.append(gen_src)

    args = ctx.actions.args()
    args.add("/nologo")
    for inc in include_dirs:
        args.add("/I", inc)
    for d in defines:
        args.add("/d", d)
    args.add("/fo" + out.path)  # /fo must be joined
    args.add(ctx.file.src.path)

    ctx.actions.run(
        executable = ctx.executable.rc,  # your wrapper
        arguments = [args],
        inputs = [ctx.file.src] + ctx.files.resources,
        outputs = [out],
        mnemonic = "WindowsRC",
    )

    li = cc_common.create_linker_input(
        owner = ctx.label,
        user_link_flags = [out.path],  # put .res on the link line
        additional_inputs = depset([out]),  # and declare it as a link input
    )
    lc = cc_common.create_linking_context(linker_inputs = depset([li]))
    new_cc_shared_info = CcSharedLibraryInfo(
        dynamic_deps = depset(),
        exports = [],
        linker_input = li,
        link_once_static_libs = {},
    )

    return [DefaultInfo(files = depset([out])), CcInfo(linking_context = lc), new_cc_shared_info]

windows_rc_rule = rule(
    implementation = _rc_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = [".rc"]),
        "resources": attr.label_list(allow_files = True),
        "defines": attr.string_list(),
        "rc": attr.label(executable = True, cfg = "exec", allow_files = True),
        "windows_version_minimal": attr.label(),
        "_os_windows": attr.label(
            default = Label("@platforms//os:windows"),
            providers = [platform_common.ConstraintValueInfo],
        ),
    },
    provides = [CcInfo],
    fragments = ["cpp"],
)

def windows_rc(name, src, manifest_in = None, icon = None):
    if manifest_in:
        # Turn "foo.manifest.in" into "foo.manifest"
        out_manifest = manifest_in[:-3] if manifest_in.endswith(".in") else manifest_in

        generate_config_header(
            name = name + "_manifest_gen",
            checks = "//src/mongo/util:version_constants_gen.py",
            cpp_defines = [],
            cpp_linkflags = [],
            cpp_opts = [],
            extra_definitions = {
                "MONGO_DISTMOD": "$(MONGO_DISTMOD)",
                "MONGO_VERSION": "$(MONGO_VERSION)",
                "GIT_COMMIT_HASH": "$(GIT_COMMIT_HASH)",
            } | select({
                "//bazel/config:js_engine_mozjs": {"js_engine_ver": "mozjs"},
                "//conditions:default": {"js_engine_ver": "none"},
            }) | select({
                "//bazel/config:tcmalloc_google_enabled": {"MONGO_ALLOCATOR": "tcmalloc-google"},
                "//bazel/config:tcmalloc_gperf_enabled": {"MONGO_ALLOCATOR": "tcmalloc-gperf"},
                "//conditions:default": {"MONGO_ALLOCATOR": "system"},
            }) | select({
                "//bazel/config:build_enterprise_enabled": {"build_enterprise_enabled": "1"},
                "//conditions:default": {},
            }),
            logfile = name + "_manifest_gen.log",
            output = out_manifest,
            template = manifest_in,
        )

    resources = ["//src/mongo/util:rc_constants_gen"]
    if manifest_in:
        resources.append(name + "_manifest_gen")
    if icon:
        resources.append(icon)

    windows_rc_rule(
        name = name,
        src = src,
        resources = resources,
        rc = select({
            "@platforms//os:windows": "@mongo_windows_toolchain//:rc",
            "//conditions:default": None,
        }),
        windows_version_minimal = "//bazel/config:win_min_version",
    )
