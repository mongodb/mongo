"""Exercise native Windows compiler/linker features without a Windows SDK."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load(":mongo_windows_cc_toolchain_config.bzl", "mongo_windows_cc_toolchain_config")

def _debug_policy_test_impl(ctx):
    env = unittest.begin(ctx)
    toolchain = ctx.attr.toolchain[cc_common.CcToolchainInfo]
    features = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = toolchain,
        requested_features = [ctx.attr.mode],
    )
    compile_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = ACTION_NAMES.cpp_compile,
        variables = cc_common.create_compile_variables(
            cc_toolchain = toolchain,
            feature_configuration = features,
            source_file = "test.cpp",
            output_file = "test.obj",
        ),
    )
    link_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = features,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = cc_common.create_link_variables(
            cc_toolchain = toolchain,
            feature_configuration = features,
            output_file = "test.exe",
        ),
    )
    asserts.equals(env, ctx.attr.symbols, "/Z7" in compile_flags)
    asserts.equals(env, ctx.attr.symbols, any([flag.startswith("/DEBUG") for flag in link_flags]))
    asserts.equals(env, ctx.attr.symbols, cc_common.is_enabled(feature_configuration = features, feature_name = "generate_pdb_file"))
    if ctx.attr.mode in ["dbg", "fastbuild"]:
        asserts.true(env, "/Od" in compile_flags)
        asserts.true(env, "/INCREMENTAL:NO" in link_flags)
    return unittest.end(env)

_debug_policy_test = rule(
    _debug_policy_test_impl,
    attrs = {
        "_impl_name": attr.string(default = "debug_policy_test"),
        "toolchain": attr.label(providers = [cc_common.CcToolchainInfo]),
        "mode": attr.string(),
        "symbols": attr.bool(),
    },
    fragments = ["cpp"],
    test = True,
    toolchains = ["@bazel_skylib//toolchains/unittest:toolchain_type"],
)

def debug_policy_test_suite(name):
    native.filegroup(name = name + "_empty", srcs = [])
    tests = []
    for symbols in [False, True]:
        suffix = "symbols" if symbols else "no_symbols"
        config = name + "_config_" + suffix
        toolchain = name + "_toolchain_" + suffix
        mongo_windows_cc_toolchain_config(
            name = config,
            cpu = "x64_windows",
            compiler = "msvc-cl",
            toolchain_identifier = suffix,
            archiver_flags = ["/NOLOGO"],
            default_link_flags = ["/NOLOGO"],
            global_defines = ["WIN32"],
            tool_paths = {tool: "unused.exe" for tool in ["gcc", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip"]},
            dbg_mode_debug_flag = "/DEBUG:FULL",
            fastbuild_mode_debug_flag = "/DEBUG:FASTLINK",
            debug_symbols = symbols,
            tags = ["manual"],
        )
        cc_toolchain(
            name = toolchain,
            toolchain_identifier = suffix,
            toolchain_config = ":" + config,
            all_files = ":" + name + "_empty",
            ar_files = ":" + name + "_empty",
            as_files = ":" + name + "_empty",
            compiler_files = ":" + name + "_empty",
            dwp_files = ":" + name + "_empty",
            linker_files = ":" + name + "_empty",
            objcopy_files = ":" + name + "_empty",
            strip_files = ":" + name + "_empty",
            tags = ["manual"],
        )
        for mode in ["dbg", "fastbuild", "opt"]:
            test = name + "_" + mode + "_" + suffix
            _debug_policy_test(
                name = test,
                toolchain = ":" + toolchain,
                symbols = symbols,
                mode = mode,
            )
            tests.append(test)
    native.test_suite(name = name, tests = tests)
