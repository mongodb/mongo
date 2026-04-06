"""WASI Preview2 (wasm32-wasip2) Bazel C/C++ toolchain config (wasi-sdk based)."""

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
    "all_c_compile_actions",
    "all_cpp_compile_actions",
)
load(
    "//bazel/toolchains/cc/mongo_linux:mongo_linux_cc_toolchain_config.bzl",
    "all_link_actions",
)

def _wasi_cc_toolchain_config_wasip2_impl(ctx):
    # We must use action configs instead of tool paths because of the external dependency.
    # This defines the binaries we use.
    action_configs = [
        action_config(
            action_name = name,
            enabled = True,
            tools = [tool(tool = ctx.executable.clangpp)],
        )
        for name in all_cpp_compile_actions
    ] + [
        action_config(
            action_name = name,
            enabled = True,
            tools = [tool(tool = ctx.executable.clang)],
        )
        for name in all_c_compile_actions
    ] + [
        action_config(
            action_name = ACTION_NAMES.cpp_link_static_library,
            implies = ["archiver_flags"],
            enabled = True,
            tools = [tool(tool = ctx.executable.ar)],
        ),
    ] + [
        action_config(
            action_name = name,
            enabled = True,
            tools = [tool(tool = ctx.executable.clangpp)],
        )
        for name in all_link_actions
    ]

    # /usr/bin/false tool paths. Action configs will do their work instead.
    tool_paths = [
        tool_path(name = "gcc", path = "/usr/bin/false"),
        tool_path(name = "ar", path = "/usr/bin/false"),
        tool_path(name = "ld", path = "/usr/bin/false"),
        tool_path(name = "cpp", path = "/usr/bin/false"),
        tool_path(name = "dwp", path = "/usr/bin/false"),
        tool_path(name = "gcov", path = "/usr/bin/false"),
        tool_path(name = "nm", path = "/usr/bin/false"),
        tool_path(name = "objdump", path = "/usr/bin/false"),
        tool_path(name = "strip", path = "/usr/bin/false"),
    ]

    # Compilation flags
    compile_flags = feature(
        name = "compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = all_cpp_compile_actions + all_c_compile_actions,
            flag_groups = [flag_group(flags = [
                "--target=wasm32-wasip2",
                "-no-canonical-prefixes",
                # This may change depending on the repo rule used to generate it.
                # find $(bazel info output_base) -name wasi-sysroot # BASH SCRIPT
                # can be used to find it if we use a different repository rule later.
                "--sysroot={}".format("external/_main~_repo_rules~wasi_sdk/share/wasi-sysroot"),
                "-fno-common",
                "-Oz",
                "-ffunction-sections",
                "-fdata-sections",
                "-fvisibility=hidden",
                "-U_FORTIFY_SOURCE",
                "-D_FORTIFY_SOURCE=0",

                # WASI emulation shims - enable syscall emulation for POSIX APIs
                "-D_WASI_EMULATED_SIGNAL",
                "-D_WASI_EMULATED_MMAN",
                "-D_WASI_EMULATED_PROCESS_CLOCKS",
                "-D_WASI_EMULATED_GETPID",

                # WASI platform lacks syslog support
                "-DBOOST_LOG_WITHOUT_SYSLOG",
                "-Wall",
                "-Werror",

                # Warnings that are also disabled in our
                # C++ compilations
                "-Wno-unused-function",
                "-Wno-defaulted-function-deleted",
                "-Wno-unknown-pragmas",
                "-Wno-builtin-macro-redefined",
                "-Wno-unused-local-typedefs",
                "-Wno-unused-lambda-capture",
                "-Wno-deprecated-declarations",
                "-Wno-unused-const-variable",
                "-Wno-deprecated-this-capture",
                "-Wno-undefined-var-template",
                "-Wno-unused-private-field",
                "-Wno-potentially-evaluated-expression",
                "-Wno-missing-braces",
                "-Wno-tautological-constant-out-of-range-compare",
                "-Wno-tautological-constant-compare",
                "-Wno-tautological-unsigned-zero-compare",
                "-Wno-tautological-unsigned-enum-zero-compare",
                "-Wno-inconsistent-missing-override",
                "-Wno-instantiation-after-specialization",
            ])],
        )],
    )

    # CXX flags
    cxx20_flags = feature(
        name = "default_cxx20_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = ["c++-compile"],
            flag_groups = [flag_group(flags = [
                "-std=c++20",
                "-fexceptions",
                "-Wno-non-virtual-dtor",

                # Include headers for declarations needed by third-party code
                "-include",
                "wasm32-wasip2/assert.h",  # assert() for Abseil
                "-include",
                "stdlib.h",  # malloc/free for fmt
            ])],
        )],
    )

    # Linker flags
    link_flags = feature(
        name = "default_link_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = all_link_actions,
            flag_groups = [flag_group(flags = [
                "-Wl,--gc-sections",
                "-Wl,--strip-all",
                "-lc++",
                "-lc++abi",
                "-lwasi-emulated-signal",
                "-lwasi-emulated-mman",
                "-lwasi-emulated-process-clocks",
                "-lwasi-emulated-getpid",
            ])],
        )],
    )

    # Include path features (mirrors the Linux toolchain).
    # Without these, external-repo headers (like Abseil) are only added via
    # -iquote (quoted includes) and <angled> includes fail.
    all_compile = [
        ACTION_NAMES.preprocess_assemble,
        ACTION_NAMES.linkstamp_compile,
        ACTION_NAMES.c_compile,
        ACTION_NAMES.cpp_compile,
        ACTION_NAMES.cpp_header_parsing,
        ACTION_NAMES.cpp_module_compile,
        ACTION_NAMES.clif_match,
        ACTION_NAMES.objc_compile,
        ACTION_NAMES.objcpp_compile,
    ]

    include_paths_feature = feature(
        name = "include_paths",
        enabled = True,
        flag_sets = [flag_set(
            actions = all_compile,
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
        )],
    )

    external_include_paths_feature = feature(
        name = "external_include_paths",
        enabled = True,
        flag_sets = [flag_set(
            actions = all_compile,
            flag_groups = [
                flag_group(
                    flags = ["-isystem", "%{external_include_paths}"],
                    iterate_over = "external_include_paths",
                    expand_if_available = "external_include_paths",
                ),
            ],
        )],
    )

    # WASI SDK paths (relative to execroot), matching the default search order
    # reported by: wasm32-wasip2-clang++ -v -x c++ /dev/null -fsyntax-only
    wasi_sdk = "external/_main~_repo_rules~wasi_sdk"
    wasi_sysroot = wasi_sdk + "/share/wasi-sysroot"

    # Construct the toolchain.
    return [cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        action_configs = action_configs,
        tool_paths = tool_paths,
        toolchain_identifier = "wasi_sdk_cc_wasip2",
        host_system_name = "local",
        target_system_name = "wasi",
        target_cpu = "wasm32",
        target_libc = "wasi",
        compiler = "clang",
        abi_version = "none",
        abi_libc_version = "wasi",
        builtin_sysroot = wasi_sysroot,
        cxx_builtin_include_directories = [
            wasi_sysroot + "/include/wasm32-wasip2/c++/v1",
            wasi_sysroot + "/include/c++/v1",
            wasi_sdk + "/lib/clang/21/include",
            wasi_sysroot + "/include/wasm32-wasip2",
            wasi_sysroot + "/include",
        ],
        features = [
            compile_flags,
            cxx20_flags,
            link_flags,
            include_paths_feature,
            external_include_paths_feature,
            feature(name = "archive_param_file", enabled = True),
            feature(name = "supports_dynamic_linker", enabled = False),
            # Override Bazel's built-in coverage feature with a no-op.
            # The WASI SDK does not ship libclang_rt.profile.a, so coverage
            # instrumentation cannot work on wasm32 targets.  Without this
            # explicit (empty) feature, `bazel coverage` injects the default
            # -fprofile-instr-generate/-fcoverage-mapping flags, which cause
            # a link failure.
            feature(name = "coverage"),
        ],
    )]

wasi_cc_toolchain_config_wasip2 = rule(
    # Defines the rule for creating this toolchain.
    implementation = _wasi_cc_toolchain_config_wasip2_impl,

    # Pass tools by name for convenience.
    attrs = {
        "clang": attr.label(
            executable = True,
            cfg = "exec",
            allow_files = True,
            default = Label("@wasi_sdk//:wasm32-wasip2-clang"),
        ),
        "clangpp": attr.label(
            executable = True,
            cfg = "exec",
            allow_files = True,
            default = Label("@wasi_sdk//:wasm32-wasip2-clang++"),
        ),
        "ar": attr.label(
            executable = True,
            cfg = "exec",
            allow_files = True,
            default = Label("@wasi_sdk//:llvm-ar"),
        ),
    },
    provides = [CcToolchainConfigInfo],
)
