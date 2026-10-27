"""Conservative local reservations for Linux objcopy actions."""

DebugLinkInfo = provider(fields = ["objects", "archives", "lto", "unlimited_debug_info"])

_DEBUG_DEPENDENCY_ATTRIBUTES = ["deps", "implementation_deps", "binary_with_debug", "cc_shared_library", "shared_archive"]

def _link_info(arguments, unlimited_debug_info = False):
    return DebugLinkInfo(
        objects = len([arg for arg in arguments if arg.endswith(".o")]),
        # Archives can hide a large number of objects, including Rust crates.
        archives = any([arg.endswith((".a", ".rlib")) and not arg.startswith("-") for arg in arguments]),
        lto = any([arg.startswith("-flto") for arg in arguments]),
        unlimited_debug_info = unlimited_debug_info,
    )

def _debug_link_info_aspect_impl(target, _ctx):
    dependencies = []
    for name in _DEBUG_DEPENDENCY_ATTRIBUTES:
        dependency = getattr(_ctx.rule.attr, name, None)
        if type(dependency) == "list":
            dependencies.extend(dependency)
        elif dependency != None:
            dependencies.append(dependency)

    # Libraries and debug wrappers must preserve this flag even when they have
    # no executable link action of their own. A dependency can enable unlimited
    # type information independently of the final program's compiler options.
    unlimited_debug_info = "clang_fno_limit_debug_info" in _ctx.features or "clang_fno_limit_debug_info" in getattr(_ctx.rule.attr, "features", []) or any([
        "-fno-limit-debug-info" in action.argv
        for action in getattr(target, "actions", [])
        if action.mnemonic == "CppCompile"
    ]) or any([
        dependency[DebugLinkInfo].unlimited_debug_info
        for dependency in dependencies
        if DebugLinkInfo in dependency
    ])

    files = target[DefaultInfo].files.to_list()
    links = [
        action
        for action in getattr(target, "actions", [])
        if action.mnemonic == "CppLink" and any([output in files for output in action.outputs.to_list()])
    ]
    if len(links) != 1:
        return [DebugLinkInfo(objects = 0, archives = False, lto = False, unlimited_debug_info = unlimited_debug_info)]
    return [_link_info(links[0].argv, unlimited_debug_info)]

debug_link_info_aspect = aspect(
    implementation = _debug_link_info_aspect_impl,
    attr_aspects = _DEBUG_DEPENDENCY_ATTRIBUTES,
)

# Measured on Linux/aarch64 with toolchain v5 LLVM 19.1.7, split DWARF,
# dbg_level=1, opt=off (2026-10). Maximum RSS in MiB over three executions of
# each exact objcopy command used by separate_debug.bzl:
#
#   binary                          objects   binary MiB   extract   strip
#   base_test                          1086           76       143     195
#   pretty_printer_test_program          842           48        93     132
#   mongobridge                        4331          461       754     941
#   mongotmock                         5359          707      1220    1418
#   db_repl_coordinator_test           6772         1175      2042    2284
#   mongos (+ Rust archives)           6047         1246      1991    2336
#   mongod (+ Rust archives)           7243         1646      2789    3169
#   extension shared library             --           43       100     120
#
# /usr/bin/time -f '%M,%U,%S,%e,%P' measures the single objcopy process; unlike
# linking, no compiler driver or child linker obscures its peak. Both operations
# use one CPU. Strip includes --add-gnu-debuglink and can use MORE memory than
# extraction. The split profile leaves at least 2.2x headroom over these peaks.
# Shared libraries retain Bazel's default (250 MiB), over 2x measured peaks.
# GDB indexing is a separate, threaded workload and retains its old budgets.
#
# Count the original link's objects, not the debug action's inputs: those consist
# of one binary plus the entire toolchain, regardless of the binary's size.
# Additional configuration measurements (same host/tool, three executions):
#
#   configuration / binary                         binary MiB   extract   strip
#   dbg, split DWARF / base_test                          116       217     187
#   opt, split DWARF / base_test                           62       130      93
#   evg, full DWARF / base_test                           122       248     125
#   evg, full DWARF / db_repl_coordinator_test            2504      4837    2355
#   dbg, full DWARF, g3, uncompressed / base_test          153       290     187
#   dbg, full DWARF, g3, uncompressed / mongobridge        1060      1955     943
#   dbg, full DWARF, g3, uncompressed / db_repl_test       2927      5533    2451
#   dbg, full DWARF, g3, uncompressed / mongod             3534      6566    3164
#   opt, distributed ThinLTO, full DWARF / base_test         90       197     103
#   dbg_aubsan, full DWARF, dynamic / base_test              26        63      48
#   dbg_aubsan, full DWARF / libbase                        43        98      70
#   dbg, full DWARF, g3, unlimited type info / base_test    180       343     185
#
# Use the conservative profile for ordinary configs outside the default
# split-DWARF configuration, including dbg/opt/evg. Full DWARF more than doubles
# large-program extraction RSS. Compile-time compression does not bound this:
# the v5 linker emits --compress-debug-sections=none. Keep LTO, instrumentation,
# unlimited type info, unknown producers, and unmeasured sizes on the fallback;
# only small binaries were measured with those first three configurations.
# Opaque archives also use the fallback: their members can push the total object
# count beyond the measured range, regardless of the number of visible .o inputs.
# The conservative profile leaves at least 2.4x headroom for measured programs.

def _resources(memory):
    return {"cpu": 1, "memory": memory, "local_test": 1}

def linux_extract_resource_set(os, numInputs):
    return _resources(20 * 1024)

def linux_strip_resource_set(os, numInputs):
    return _resources(10 * 1024)

def _small_extract_resource_set(os, numInputs):
    return _resources(1024)

def _small_strip_resource_set(os, numInputs):
    return _resources(1024)

def _medium_extract_resource_set(os, numInputs):
    return _resources(2 * 1024)

def _medium_strip_resource_set(os, numInputs):
    return _resources(3 * 1024)

def _large_extract_resource_set(os, numInputs):
    return _resources(6 * 1024)

def _large_strip_resource_set(os, numInputs):
    return _resources(8 * 1024)

def _conservative_medium_extract_resource_set(os, numInputs):
    return _resources(8 * 1024)

def _conservative_large_extract_resource_set(os, numInputs):
    return _resources(16 * 1024)

def debug_resource_sets(link_info, profile):
    extract = linux_extract_resource_set
    strip = linux_strip_resource_set
    if profile in ["split", "conservative"] and not link_info.archives and not link_info.lto and not getattr(link_info, "unlimited_debug_info", False) and link_info.objects > 0 and link_info.objects <= 8000:
        if link_info.objects <= 1500:
            extract = _small_extract_resource_set
            strip = _small_strip_resource_set
        elif link_info.objects <= 5000:
            extract = _medium_extract_resource_set if profile == "split" else _conservative_medium_extract_resource_set
            strip = _medium_strip_resource_set
        else:
            extract = _large_extract_resource_set if profile == "split" else _conservative_large_extract_resource_set
            strip = _large_strip_resource_set
    return struct(extract = extract, strip = strip)

def linux_gdb_generate_resource_set(os, numInputs):
    return {"cpu": 6, "memory": 20 * 1024, "local_test": 1}

def linux_gdb_apply_resource_set(os, numInputs):
    return {"cpu": 3, "memory": 10 * 1024, "local_test": 1}

testonly_helpers = struct(link_info = _link_info)
