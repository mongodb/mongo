load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load(":debug_resources.bzl", "DebugLinkInfo", "debug_link_info_aspect", "debug_resource_sets", "linux_gdb_apply_resource_set", "linux_gdb_generate_resource_set", "testonly_helpers")

def _assert_reservations(env, info, profile, extract_mb, strip_mb):
    resources = debug_resource_sets(info, profile)

    # Toolchain input counts must not affect a tier selected from the link.
    for inputs in [1, 20000]:
        asserts.equals(env, {"cpu": 1, "memory": extract_mb, "local_test": 1}, resources.extract("linux", inputs))
        asserts.equals(env, {"cpu": 1, "memory": strip_mb, "local_test": 1}, resources.strip("linux", inputs))

def _tiers_test_impl(ctx):
    env = unittest.begin(ctx)
    for objects, extract, strip in [
        (1, 1024, 1024),
        (1500, 1024, 1024),
        (1501, 2048, 3072),
        (5000, 2048, 3072),
        (5001, 6144, 8192),
        (8000, 6144, 8192),
    ]:
        _assert_reservations(env, DebugLinkInfo(objects = objects, archives = False, lto = False), "split", extract, strip)
    for objects, extract, strip in [
        (1, 1024, 1024),
        (1500, 1024, 1024),
        (1501, 8192, 3072),
        (5000, 8192, 3072),
        (5001, 16384, 8192),
        (8000, 16384, 8192),
    ]:
        _assert_reservations(env, DebugLinkInfo(objects = objects, archives = False, lto = False), "conservative", extract, strip)
    return unittest.end(env)

_tiers_test = unittest.make(_tiers_test_impl)

def _fallbacks_test_impl(ctx):
    env = unittest.begin(ctx)
    for objects, archives, lto, profile in [
        (0, False, False, "split"),
        (8001, False, False, "split"),
        (1, False, True, "split"),
        (1, False, True, "conservative"),
        (1, False, False, "unmeasured"),
    ]:
        _assert_reservations(env, DebugLinkInfo(objects = objects, archives = archives, lto = lto), profile, 20480, 10240)
    for profile in ["split", "conservative"]:
        _assert_reservations(env, DebugLinkInfo(objects = 1, archives = False, lto = False, unlimited_debug_info = True), profile, 20480, 10240)

    # Visible .o inputs cannot bound the number of objects in opaque archives.
    for objects in [0, 1, 1500, 1501, 5000, 5001, 8000, 8001]:
        for profile in ["split", "conservative"]:
            _assert_reservations(env, DebugLinkInfo(objects = objects, archives = True, lto = False), profile, 20480, 10240)
    return unittest.end(env)

_fallbacks_test = unittest.make(_fallbacks_test_impl)

def _arguments_test_impl(ctx):
    env = unittest.begin(ctx)
    info = testonly_helpers.link_info(["clang", "a.o", "b.pic.o", "libdependency.so", "-l:libatomic.a"])
    asserts.equals(env, 2, info.objects)
    asserts.false(env, info.archives)
    asserts.false(env, info.lto)
    for archive in ["dependency.a", "dependency.rlib"]:
        info = testonly_helpers.link_info(["clang", "a.o", archive])
        asserts.true(env, info.archives)
        for profile in ["split", "conservative"]:
            _assert_reservations(env, info, profile, 20480, 10240)
    for option in ["-flto", "-flto=thin"]:
        asserts.true(env, testonly_helpers.link_info([option]).lto)
    return unittest.end(env)

_arguments_test = unittest.make(_arguments_test_impl)

def _gdb_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, {"cpu": 6, "memory": 20480, "local_test": 1}, linux_gdb_generate_resource_set("linux", 1))
    asserts.equals(env, {"cpu": 3, "memory": 10240, "local_test": 1}, linux_gdb_apply_resource_set("linux", 1))
    return unittest.end(env)

_gdb_test = unittest.make(_gdb_test_impl)

def _producer_impl(ctx):
    binary = ctx.actions.declare_file(ctx.label.name)
    if ctx.attr.link:
        ctx.actions.run(executable = "/bin/true", arguments = ["a.o", "b.pic.o"], outputs = [binary], mnemonic = "CppLink")

        # Toolchain version probes also use CppLink; they are not binary links.
        probe = ctx.actions.declare_file(ctx.label.name + ".json")
        ctx.actions.run(executable = "/bin/true", arguments = ["ignored.o"], outputs = [probe], mnemonic = "CppLink")
    else:
        ctx.actions.write(binary, "unknown producer")
    if ctx.attr.unlimited_debug_info:
        obj = ctx.actions.declare_file(ctx.label.name + ".o")
        ctx.actions.run(executable = "/bin/true", arguments = ["-fno-limit-debug-info"], outputs = [obj], mnemonic = "CppCompile")
    return [DefaultInfo(files = depset([binary]))]

_producer = rule(implementation = _producer_impl, attrs = {"link": attr.bool(default = True), "unlimited_debug_info": attr.bool(), "deps": attr.label_list(), "implementation_deps": attr.label_list()})

def _debug_wrapper_impl(ctx):
    return [DefaultInfo(files = ctx.attr.binary_with_debug[DefaultInfo].files)]

_debug_wrapper = rule(implementation = _debug_wrapper_impl, attrs = {"binary_with_debug": attr.label()})

def _aspect_test_impl(ctx):
    env = analysistest.begin(ctx)
    info = analysistest.target_under_test(env)[DebugLinkInfo]
    asserts.equals(env, ctx.attr.objects, info.objects)
    asserts.equals(env, ctx.attr.unlimited_debug_info, info.unlimited_debug_info)
    if ctx.attr.unlimited_debug_info and info.objects:
        # A dependency flag must reach the actual reservation selection.
        for profile in ["split", "conservative"]:
            _assert_reservations(env, info, profile, 20480, 10240)
    return analysistest.end(env)

_aspect_test = analysistest.make(
    _aspect_test_impl,
    attrs = {"objects": attr.int(), "unlimited_debug_info": attr.bool()},
    extra_target_under_test_aspects = [debug_link_info_aspect],
)

_aspect_feature_test = analysistest.make(
    _aspect_test_impl,
    attrs = {"objects": attr.int(), "unlimited_debug_info": attr.bool()},
    extra_target_under_test_aspects = [debug_link_info_aspect],
    config_settings = {"//command_line_option:features": ["clang_fno_limit_debug_info"]},
)

_ConfigurationInfo = provider(fields = ["profile"])

def _configuration_impl(ctx):
    return [_ConfigurationInfo(profile = ctx.attr.profile)]

_configuration = rule(implementation = _configuration_impl, attrs = {"profile": attr.string()})

def _configuration_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.equals(env, ctx.attr.profile, analysistest.target_under_test(env)[_ConfigurationInfo].profile)
    return analysistest.end(env)

_MEASURED_CONFIGURATION = {
    "//command_line_option:fission": "yes",
    str(Label("//bazel/config:mongo_toolchain_version")): "v5",
    str(Label("//bazel/config:compiler_type")): "clang",
    str(Label("//bazel/config:opt")): "off",
    str(Label("//bazel/config:dbg_level")): "1",
    str(Label("//bazel/config:compress_debug_compile")): True,
    str(Label("//bazel/config:dtlto")): False,
    str(Label("//bazel/config:thin_lto")): False,
    str(Label("//bazel/config:asan")): False,
    str(Label("//bazel/config:tsan")): False,
    str(Label("//bazel/config:ubsan")): False,
    str(Label("//bazel/config:msan")): False,
    str(Label("//bazel/config:fsan")): False,
    str(Label("//bazel/config:lsan")): False,
    str(Label("//bazel/config:coverage")): False,
    str(Label("//bazel/config:gcov")): False,
    str(Label("//bazel/config:pgo_profile_generate")): False,
    str(Label("//bazel/config:cspgo_profile_generate")): False,
}

def _configuration_test(changes = {}):
    return analysistest.make(
        _configuration_test_impl,
        attrs = {"profile": attr.string()},
        config_settings = _MEASURED_CONFIGURATION | changes,
    )

_measured_configuration_test = _configuration_test()
_full_dwarf_configuration_test = _configuration_test({"//command_line_option:fission": "no"})
_lto_configuration_test = _configuration_test({str(Label("//bazel/config:thin_lto")): True})
_instrumented_configuration_test = _configuration_test({str(Label("//bazel/config:asan")): True})
_full_debug_configuration_test = _configuration_test({str(Label("//bazel/config:dbg_level")): "3"})
_optimized_configuration_test = _configuration_test({str(Label("//bazel/config:opt")): "on"})
_dbg_configuration_test = _configuration_test({str(Label("//bazel/config:dbg_level")): "2"})
_uncompressed_configuration_test = _configuration_test({str(Label("//bazel/config:compress_debug_compile")): False})
_gcc_configuration_test = _configuration_test({str(Label("//bazel/config:compiler_type")): "gcc"})
_dtlto_configuration_test = _configuration_test({str(Label("//bazel/config:dtlto")): True})
_evg_configuration_test = _configuration_test({
    "//command_line_option:fission": "no",
    str(Label("//bazel/config:opt")): "auto",
    str(Label("//bazel/config:dbg_level")): "2",
    str(Label("//bazel/config:evg")): True,
})
_stress_configuration_test = _configuration_test({
    "//command_line_option:fission": "no",
    str(Label("//bazel/config:dbg_level")): "3",
    str(Label("//bazel/config:compress_debug_compile")): False,
})

def _instrumentation_configuration_test(flag, fission):
    return _configuration_test({
        "//command_line_option:fission": fission,
        str(Label("//bazel/config:" + flag)): True,
    })

_lsan_split_test = _instrumentation_configuration_test("lsan", "yes")
_lsan_full_test = _instrumentation_configuration_test("lsan", "no")
_coverage_split_test = _instrumentation_configuration_test("coverage", "yes")
_coverage_full_test = _instrumentation_configuration_test("coverage", "no")
_gcov_split_test = _instrumentation_configuration_test("gcov", "yes")
_gcov_full_test = _instrumentation_configuration_test("gcov", "no")
_pgo_split_test = _instrumentation_configuration_test("pgo_profile_generate", "yes")
_pgo_full_test = _instrumentation_configuration_test("pgo_profile_generate", "no")
_cspgo_split_test = _instrumentation_configuration_test("cspgo_profile_generate", "yes")
_cspgo_full_test = _instrumentation_configuration_test("cspgo_profile_generate", "no")

_INSTRUMENTED_CONFIGURATION_TESTS = [
    ("lsan_yes", _lsan_split_test, "unmeasured"),
    ("lsan_no", _lsan_full_test, "unmeasured"),
    ("coverage_yes", _coverage_split_test, "unmeasured"),
    ("coverage_no", _coverage_full_test, "unmeasured"),
    ("gcov_yes", _gcov_split_test, "unmeasured"),
    ("gcov_no", _gcov_full_test, "unmeasured"),
    ("pgo_profile_generate_yes", _pgo_split_test, "unmeasured"),
    ("pgo_profile_generate_no", _pgo_full_test, "unmeasured"),
    ("cspgo_profile_generate_yes", _cspgo_split_test, "unmeasured"),
    ("cspgo_profile_generate_no", _cspgo_full_test, "unmeasured"),
]

def debug_resources_test_suite(name):
    _tiers_test(name = name + "_tiers")
    _fallbacks_test(name = name + "_fallbacks")
    _arguments_test(name = name + "_arguments")
    _gdb_test(name = name + "_gdb")
    for suffix, linked, objects, unlimited in [("linked", True, 2, False), ("unknown", False, 0, False), ("unlimited", True, 2, True)]:
        _producer(name = name + "_" + suffix + "_producer", link = linked, unlimited_debug_info = unlimited, tags = ["manual"])
        _aspect_test(name = name + "_" + suffix, target_under_test = ":" + name + "_" + suffix + "_producer", objects = objects, unlimited_debug_info = unlimited)
    _aspect_feature_test(name = name + "_unlimited_feature", target_under_test = ":" + name + "_linked_producer", objects = 2, unlimited_debug_info = True)
    dependency_tests = []
    for edge, prefix in [("deps", "dependency"), ("implementation_deps", "implementation_dependency")]:
        for suffix, unlimited, features in [("compile", True, []), ("feature", False, ["clang_fno_limit_debug_info"]), ("ordinary", False, [])]:
            leaf = name + "_" + prefix + "_" + suffix
            _producer(name = leaf, link = False, unlimited_debug_info = unlimited, features = features, tags = ["manual"])
            _debug_wrapper(name = leaf + "_debug", binary_with_debug = ":" + leaf, tags = ["manual"])

            # Exercise both library dependency attributes through the same
            # binary_with_debug edge used by extract_debuginfo wrappers.
            _producer(name = leaf + "_middle", link = False, tags = ["manual"], **{edge: [":" + leaf + "_debug"]})
            _producer(name = leaf + "_program", deps = [":" + leaf + "_middle"], tags = ["manual"])
            test_name = leaf + "_test"
            _aspect_test(name = test_name, target_under_test = ":" + leaf + "_program", objects = 2, unlimited_debug_info = suffix != "ordinary")
            dependency_tests.append(":" + test_name)
    _configuration(
        name = name + "_configuration",
        profile = select({
            "//bazel/config:measured_debug_processing": "split",
            "//bazel/config:conservative_debug_processing": "conservative",
            "//conditions:default": "unmeasured",
        }),
        tags = ["manual"],
    )
    configurations = [
        ("measured", _measured_configuration_test, "split"),
        ("full_dwarf", _full_dwarf_configuration_test, "conservative"),
        ("lto", _lto_configuration_test, "unmeasured"),
        ("instrumented", _instrumented_configuration_test, "unmeasured"),
        ("full_debug", _full_debug_configuration_test, "conservative"),
        ("optimized", _optimized_configuration_test, "conservative"),
        ("dbg", _dbg_configuration_test, "conservative"),
        ("uncompressed", _uncompressed_configuration_test, "conservative"),
        ("gcc", _gcc_configuration_test, "unmeasured"),
        ("dtlto", _dtlto_configuration_test, "unmeasured"),
        ("evg", _evg_configuration_test, "conservative"),
        ("stress", _stress_configuration_test, "conservative"),
    ] + _INSTRUMENTED_CONFIGURATION_TESTS
    for suffix, test, profile in configurations:
        test(
            name = name + "_" + suffix,
            target_under_test = ":" + name + "_configuration",
            profile = profile,
            target_compatible_with = ["@platforms//os:linux"],
        )
    native.test_suite(
        name = name,
        tests = [":" + name + "_" + suffix for suffix in ["tiers", "fallbacks", "arguments", "gdb", "linked", "unknown", "unlimited", "unlimited_feature"] + [case[0] for case in configurations]] + dependency_tests,
    )
