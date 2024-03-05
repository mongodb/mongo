"""Starlark bazel build configurations, see https://bazel.build/extending/config"""

# =============
# compiler_type
# =============

compiler_type_provider = provider(
    doc = "Select the compiler (e.g.: gcc)",
    fields = {"compiler_type": "Choose one of [gcc, clang]"},
)

compiler_type = rule(
    implementation = lambda ctx: compiler_type_provider(compiler_type = ctx.build_setting_value),
    build_setting = config.string(flag = True),
)

# ==========
# build_mode
# ==========

build_mode_values = ["dbg", "release", "opt_on", "opt_off", "opt_size", "opt_debug"]

build_mode_provider = provider(
    doc = "Select the overall mode of the build, e.g debug/optimized or some combination/extension of those.",
    fields = {"build_mode": "choose one of " + ".".join(build_mode_values)},
)

def build_mode_impl(ctx):
    build_mode_value = ctx.build_setting_value
    if build_mode_value not in build_mode_values:
        fail(str(ctx.label) + " build_mode allowed to take values {" + ", ".join(build_mode_values) + "} but was set to unallowed value " + build_mode_value)
    return build_mode_provider(build_mode = build_mode_value)

build_mode = rule(
    implementation = build_mode_impl,
    build_setting = config.string(flag = True),
)

# ==========
# linker
# ==========

linker_values = ["auto", "gold", "lld"]

linker_provider = provider(
    doc = "Specify the type of linker to use.",
    fields = {"linker": "choose one of " + ".".join(linker_values)},
)

def linker_impl(ctx):
    linker_value = ctx.build_setting_value
    if linker_value not in linker_values:
        fail(str(ctx.label) + " build_mode allowed to take values {" + ", ".join(linker_values) + "} but was set to unallowed value " + linker_value)
    return linker_provider(linker = linker_value)

linker = rule(
    implementation = linker_impl,
    build_setting = config.string(flag = True),
)

# =========
# gdbserver
# =========

use_gdbserver_provider = provider(
    doc = "Choose if gdbserver should be used",
    fields = ["enabled"],
)

use_gdbserver = rule(
    implementation = lambda ctx: use_gdbserver_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# libunwind
# =========

libunwind_values = ["auto", "on", "off"]

libunwind_provider = provider(
    doc = "Enable libunwind for backtraces (use \"auto\" to enable only if its available on the current platform)",
    fields = {"libunwind": "choose one of " + ".".join(libunwind_values)},
)

def libunwind_impl(ctx):
    libunwind_value = ctx.build_setting_value
    if libunwind_value not in libunwind_values:
        fail(str(ctx.label) + " libunwind allowed to take values {" + ", ".join(libunwind_values) + "} but was set to unallowed value " + libunwind_value)
    return libunwind_provider(libunwind = libunwind_value)

libunwind = rule(
    implementation = libunwind_impl,
    build_setting = config.string(flag = True),
)

# =========
# spider_monkey_dbg
# =========

spider_monkey_dbg_provider = provider(doc = "Enable SpiderMonkey debug mode.", fields = ["enabled"])

spider_monkey_dbg = rule(
    implementation = lambda ctx: spider_monkey_dbg_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# allocator
# =========

allocator_values = ["auto", "system", "tcmalloc"]

allocator_provider = provider(
    doc = "Allocator to use (use \"auto\" for best choice for current platform)",
    fields = {"allocator": "choose one of " + ".".join(allocator_values)},
)

def allocator_impl(ctx):
    allocator_value = ctx.build_setting_value
    if allocator_value not in allocator_values:
        fail(str(ctx.label) + " allocator allowed to take values {" + ", ".join(allocator_values) + "} but was set to unallowed value " + allocator_value)
    return allocator_provider(allocator = allocator_value)

allocator = rule(
    implementation = allocator_impl,
    build_setting = config.string(flag = True),
)

# =========
# lldb-server
# =========

use_lldbserver_provider = provider(
    doc = "Choose if lldbserver should be used",
    fields = ["enabled"],
)

use_lldbserver = rule(
    implementation = lambda ctx: use_lldbserver_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# wait_for_debugger
# =========

use_wait_for_debugger_provider = provider(
    doc = "Wait for debugger attach on process startup",
    fields = ["enabled"],
)

use_wait_for_debugger = rule(
    implementation = lambda ctx: use_wait_for_debugger_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# ocsp-stapling
# =========

use_ocsp_stapling_provider = provider(
    doc = "Enable OCSP Stapling on servers",
    fields = ["enabled"],
)

use_ocsp_stapling = rule(
    implementation = lambda ctx: use_ocsp_stapling_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# disable-ref-track
# =========

use_disable_ref_track_provider = provider(
    doc = """Disables runtime tracking of REF state changes for pages within wiredtiger.
    Tracking the REF state changes is useful for debugging but there is a small performance cost.""",
    fields = ["enabled"],
)

use_disable_ref_track = rule(
    implementation = lambda ctx: use_disable_ref_track_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# wiredtiger
# =========

use_wiredtiger_provider = provider(
    doc = """Enable wiredtiger""",
    fields = ["enabled"],
)

use_wiredtiger = rule(
    implementation = lambda ctx: use_wiredtiger_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# glibcxx-debug
# =========

use_glibcxx_debug_provider = provider(
    doc = """Enable the glibc++ debug implementations of the C++ standard libary""",
    fields = ["enabled"],
)

use_glibcxx_debug = rule(
    implementation = lambda ctx: use_glibcxx_debug_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# libc++
# =========

use_libcxx_provider = provider(
    doc = """use libc++ (experimental, requires clang)""",
    fields = ["enabled"],
)

use_libcxx = rule(
    implementation = lambda ctx: use_libcxx_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# grpc
# =========

build_grpc_provider = provider(
    doc = """Enable building grpc and protobuf compiler. This has no effect on non-linux operating systems.""",
    fields = ["enabled"],
)

build_grpc = rule(
    implementation = lambda ctx: build_grpc_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# sanitize
# =========

sanitize_provider = provider(
    doc = "enable selected sanitizers",
    fields = ["enabled"],
)

asan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

fsan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)
lsan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)
msan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

tsan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

ubsan = rule(
    implementation = lambda ctx: sanitize_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# separate_debug
# =========

separate_debug_provider = provider(
    doc = "Enable splitting deubg info into a separate file (e.g. '.debug')",
    fields = ["enabled"],
)

separate_debug = rule(
    implementation = lambda ctx: separate_debug_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# enable-http-client
# =========

http_client_provider = provider(
    doc = "Enable HTTP client",
    fields = ["enabled"],
)

http_client = rule(
    implementation = lambda ctx: linkstatic_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# linkstatic
# =========

linkstatic_provider = provider(
    doc = "Configures the entire build to link statically. Disabling this on windows is not supported.",
    fields = ["enabled"],
)

linkstatic = rule(
    implementation = lambda ctx: linkstatic_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# use-diagnostic-latches
# =========

use_diagnostic_latches_provider = provider(
    doc = "Enable annotated Mutex types.",
    fields = ["enabled"],
)

use_diagnostic_latches = rule(
    implementation = lambda ctx: use_diagnostic_latches_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# shared_archive
# =========

shared_archive_provider = provider(
    doc = "Enable generating a shared archive file for each shared library (e.g. '.so.a')",
    fields = ["enabled"],
)

shared_archive = rule(
    implementation = lambda ctx: shared_archive_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# detect_odr_violations
# =========

detect_odr_violations_provider = provider(
    doc = """Have the linker try to detect ODR violations, if supported""",
    fields = ["enabled"],
)

detect_odr_violations = rule(
    implementation = lambda ctx: detect_odr_violations_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# build_enterprise_module
# =========

# Original documentation is:
#   Comma-separated list of modules to build. Empty means none. Default is all.
# As Bazel will not support the module building in the same way as Scons, the only
# module is supported at present is the enterprise
# more: https://mongodb.slack.com/archives/C05V4F6GZ6J/p1705687513581639

build_enterprise_provider = provider(
    doc = """Build enterprise module""",
    fields = ["enabled"],
)

build_enterprise = rule(
    implementation = lambda ctx: build_enterprise_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# streams-release-build
# =========
streams_release_build_provider = provider(
    doc = """If set, will include the enterprise streams module in a release build.""",
    fields = ["enabled"],
)

streams_release_build = rule(
    implementation = lambda ctx: streams_release_build_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# visibility-support
# =========

visibility_support_values = ["auto", "on", "off"]

visibility_support_provider = provider(
    doc = "Enable visibility annotations",
    fields = ["type"],
)

def visibility_support_impl(ctx):
    visibility_support_value = ctx.build_setting_value
    if visibility_support_value not in visibility_support_values:
        fail(str(ctx.label) + " visibility-support allowed to take values {" + ", ".join(visibility_support_values) + "} but was set to unallowed value " + visibility_support_value)
    return visibility_support_provider(type = visibility_support_value)

visibility_support = rule(
    implementation = visibility_support_impl,
    build_setting = config.string(flag = True),
)
