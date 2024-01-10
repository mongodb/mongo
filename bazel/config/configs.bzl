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

# =========
# gdbserver
# =========

use_gdbserver_provider = provider(
    doc = "Choose if gdbserver should be used",
    fields = ["type"],
)

use_gdbserver = rule(
    implementation = lambda ctx: use_gdbserver_provider(type = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
)

# =========
# libunwind
# =========

use_libunwind_provider = provider(fields = ["enabled"])

use_libunwind = rule(
    implementation = lambda ctx: use_libunwind_provider(enabled = ctx.build_setting_value),
    build_setting = config.bool(flag = True),
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
    fields = ["type"],
)

use_lldbserver = rule(
    implementation = lambda ctx: use_lldbserver_provider(type = ctx.build_setting_value),
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
