"""Rust toolchains that layer mongo's rustc flags over the rules_rust toolchains.

The rust.toolchain() module extension can't take select()s or per-target conditions, so
rather than redeclaring every rust_toolchain (and every tool label in it), wrap the
toolchain rules_rust already generated and append our flags to its ToolchainInfo. Tool
versions and labels keep coming from the extension in MODULE.bazel.

The flags reach every rust crate built for the target, including third-party crates, but
not exec-configuration tools: rules_rust only applies a toolchain's extra_rustc_flags
outside of exec builds. The opt and debug levels apply to exec builds too, as the
equivalent C++ toolchain settings do.
"""

load("//bazel/toolchains/cc:mongo_toolchain_selects.bzl", "DEBUG_LEVEL")

# Mirrors the mtune_march feature in
# //bazel/toolchains/cc/mongo_linux:mongo_linux_cc_toolchain_config.bzl so rust code is
# built for the same baseline CPU as the C++ it links with. Keyed by rust target triple;
# keep in sync with mtune_flags there.
MONGO_RUSTC_TUNING_FLAGS = {
    # -march=armv8.2-a -mtune=generic. rustc only exposes the v8.2a umbrella feature as
    # unstable and warns on every crate, so enable the extensions it implies directly.
    # These are the same features clang turns on for armv8.2-a.
    "aarch64-unknown-linux-gnu": [
        "-Ctarget-feature=+crc,+lse,+rdm,+ras,+lor,+pan,+vh,+dpb",
        "-Ztune-cpu=generic",
    ],
    # -mcpu=power8 -mtune=power8 -mcmodel=medium
    "powerpc64le-unknown-linux-gnu": [
        "-Ctarget-cpu=pwr8",
        "-Ztune-cpu=pwr8",
        "-Ccode-model=medium",
    ],
    # -march=z196 -mtune=zEC12
    "s390x-unknown-linux-gnu": [
        "-Ctarget-cpu=z196",
        "-Ztune-cpu=zEC12",
    ],
    # -march=sandybridge -mtune=generic. There is no rustc equivalent of
    # -mprefer-vector-width=128 that doesn't warn on every crate, so it is omitted.
    "x86_64-unknown-linux-gnu": [
        "-Ctarget-cpu=sandybridge",
        "-Ztune-cpu=generic",
    ],
}

# Same --//bazel/config:opt settings that pick the C++ optimization_level in
# //bazel/toolchains/cc:mongo_custom_features.bzl, mapped to their closest rustc levels.
# These toolchains only target linux, where the C++ compiler is always gcc or clang, so
# the plain opt_* settings are equivalent to the gcc_or_clang_opt_* ones used there.
MONGO_RUSTC_OPT_LEVEL = select({
    # -Og. rustc has no debug-friendly level; 1 is the lightest optimizing one.
    "//bazel/config:opt_debug": "1",
    # -O0
    "//bazel/config:opt_off": "0",
    # -O2
    "//bazel/config:opt_on": "2",
    # -Os
    "//bazel/config:opt_size": "s",
})

# Mirrors general_gcc_or_clang_options in the linux cc toolchain config, which is always
# enabled.
MONGO_RUSTC_COMMON_FLAGS = [
    # -fno-omit-frame-pointer, so profilers and stack walkers work through rust frames.
    # rustc otherwise omits them on x86_64, s390x and ppc64le, and in leaf functions on
    # aarch64.
    "-Cforce-frame-pointers=yes",
    # -fstack-protector-strong
    "-Zstack-protector=strong",
] + select({
    # Same settings as DWARF_VERSION in
    # //bazel/toolchains/cc/mongo_linux:mongo_toolchain.BUILD.tmpl.
    "//bazel/config:dwarf_version_4_linux": ["-Zdwarf-version=4"],
    "//bazel/config:dwarf_version_5_linux": ["-Zdwarf-version=5"],
    "//conditions:default": [],
})

# The C++ -g level, from the same DEBUG_LEVEL select the C++ toolchains use, mapped to
# rustc's -Cdebuginfo. Keyed by the C++ level. The toolchain applies this to every crate,
# so it stays at line tables: enough for symbolized backtraces, without the size cost of
# full type/variable info across large third-party crates. First-party crates get full
# debug info on top of this through MONGO_RUSTC_FIRST_PARTY_FLAGS.
_RUSTC_DEBUG_INFO = {
    0: "0",
    1: "line-tables-only",
    2: "line-tables-only",
    3: "line-tables-only",
}

# Flags for crates built by the mongo_rust_* macros in //bazel:mongo_src_rules.bzl, i.e.
# our own code. rules_rust passes a target's rustc_flags after the toolchain's
# -Cdebuginfo, and rustc takes the last value, so at -g2/-g3 this raises our crates to
# full debug info while third-party crates keep _RUSTC_DEBUG_INFO's line tables.
MONGO_RUSTC_FIRST_PARTY_FLAGS = select({
    "//bazel/toolchains/rust:first_party_full_debug_info_dbg_level_2": ["-Cdebuginfo=2"],
    # Rust has no debug level 3: 2 ("full") is rustc's highest. C++ -g3 only adds
    # preprocessor macro definitions, which rust doesn't have.
    "//bazel/toolchains/rust:first_party_full_debug_info_dbg_level_3": ["-Cdebuginfo=2"],
    "//conditions:default": [],
})

MONGO_RUSTC_DISABLE_DEBUG_SYMBOLS = select({
    "//bazel/config:debug_symbols_disabled": True,
    "//conditions:default": False,
})

# Deprecated struct methods that dir() reports but ToolchainInfo() won't accept back.
_STRUCT_METHODS = ["to_json", "to_proto"]

def _mongo_rust_toolchain_impl(ctx):
    base = ctx.attr.toolchain[platform_common.ToolchainInfo]
    fields = {
        name: getattr(base, name)
        for name in dir(base)
        if name not in _STRUCT_METHODS
    }
    fields["extra_rustc_flags"] = base.extra_rustc_flags + ctx.attr.extra_rustc_flags

    # rules_rust picks -Copt-level and -Cdebuginfo from compilation_mode_opts[-c mode],
    # but ours are chosen by //bazel/config flags independently of -c, so apply them to
    # every mode.
    debug_info = None
    if ctx.attr.disable_debug_symbols:
        debug_info = "0"
    elif ctx.attr.debug_level >= 0:
        debug_info = _RUSTC_DEBUG_INFO[ctx.attr.debug_level]
    compilation_mode_opts = {}
    for mode, opts in base.compilation_mode_opts.items():
        strip_level = opts.strip_level
        if debug_info and debug_info != "0":
            # rules_rust strips debuginfo at compile time in opt by default, which would
            # throw away the debug info asked for. C++ doesn't strip at compile time either.
            strip_level = "none"
        compilation_mode_opts[mode] = struct(
            debug_info = debug_info or opts.debug_info,
            opt_level = ctx.attr.opt_level or opts.opt_level,
            strip_level = strip_level,
        )
    fields["compilation_mode_opts"] = compilation_mode_opts
    return [
        platform_common.ToolchainInfo(**fields),
        ctx.attr.toolchain[platform_common.TemplateVariableInfo],
    ]

mongo_rust_toolchain = rule(
    implementation = _mongo_rust_toolchain_impl,
    attrs = {
        "debug_level": attr.int(
            default = -1,
            doc = "The C++ -g level to match with -Cdebuginfo. -1 keeps the value from the wrapped toolchain.",
        ),
        "disable_debug_symbols": attr.bool(
            doc = "Build without debug info regardless of debug_level.",
        ),
        "extra_rustc_flags": attr.string_list(
            doc = "Flags appended to the wrapped toolchain's extra_rustc_flags.",
        ),
        "opt_level": attr.string(
            doc = "rustc opt-level for every compilation mode. Empty keeps the wrapped toolchain's.",
        ),
        "toolchain": attr.label(
            doc = "A rules_rust rust_toolchain target to extend.",
            mandatory = True,
            providers = [platform_common.ToolchainInfo],
        ),
    },
    doc = "Extends a rules_rust rust_toolchain with extra target rustc flags.",
)

# Execution hosts rules_rust generates toolchains for that we build on, by the name
# prefix of their tool repos.
_EXEC_HOSTS = {
    "rust_linux_aarch64": ["@platforms//cpu:aarch64", "@platforms//os:linux"],
    "rust_linux_powerpc64le": ["@platforms//cpu:ppc64le", "@platforms//os:linux"],
    "rust_linux_s390x": ["@platforms//cpu:s390x", "@platforms//os:linux"],
    "rust_linux_x86_64": ["@platforms//cpu:x86_64", "@platforms//os:linux"],
}

_TARGETS = {
    "aarch64-unknown-linux-gnu": ["@platforms//cpu:aarch64", "@platforms//os:linux"],
    "powerpc64le-unknown-linux-gnu": ["@platforms//cpu:ppc64le", "@platforms//os:linux"],
    "s390x-unknown-linux-gnu": ["@platforms//cpu:s390x", "@platforms//os:linux"],
    "x86_64-unknown-linux-gnu": ["@platforms//cpu:x86_64", "@platforms//os:linux"],
}

def mongo_rust_toolchain_repos():
    """Tool repos the toolchains below wrap. MODULE.bazel must use_repo() all of these."""
    return [
        "{}__{}__nightly_tools".format(exec_host, target)
        for exec_host in _EXEC_HOSTS
        for target in _TARGETS
    ]

def mongo_rust_toolchains():
    """Declares a mongo toolchain for every exec host x linux target pair.

    Pairs not covered here (wasm, darwin targets, windows hosts) fall through to the
    rules_rust toolchains, which are registered after ours.
    """
    for exec_host, exec_constraints in _EXEC_HOSTS.items():
        for target, target_constraints in _TARGETS.items():
            name = "{}__{}".format(exec_host, target)
            mongo_rust_toolchain(
                name = name + "_impl",
                toolchain = "@{}__{}__nightly_tools//:rust_toolchain".format(exec_host, target),
                extra_rustc_flags = MONGO_RUSTC_TUNING_FLAGS[target] + MONGO_RUSTC_COMMON_FLAGS,
                opt_level = MONGO_RUSTC_OPT_LEVEL,
                debug_level = DEBUG_LEVEL,
                disable_debug_symbols = MONGO_RUSTC_DISABLE_DEBUG_SYMBOLS,
                # Keep `bazel build //...` from fetching every host's rust distribution.
                tags = ["manual"],
            )
            native.toolchain(
                name = name,
                exec_compatible_with = exec_constraints,
                target_compatible_with = target_constraints,
                target_settings = ["@rules_rust//rust/toolchain/channel:nightly"],
                toolchain = ":" + name + "_impl",
                toolchain_type = "@rules_rust//rust:toolchain_type",
            )
