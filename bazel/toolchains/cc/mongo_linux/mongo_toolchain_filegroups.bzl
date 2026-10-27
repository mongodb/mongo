"""Action-specific inputs for native and composed Linux toolchain archives."""

def _archive_files(patterns, roots = [""]):
    return native.glob([root + pattern for root in roots for pattern in patterns])

def _tools(version, tools, host = False):
    # Include both the stow installation and its vN aliases: local sandboxes
    # preserve symlinks, whereas remote execution can materialize their targets.
    # Cross output wrappers also require their .exec and .native siblings.
    dirs = ["host_tools/{}/bin/".format(version)] if host else [
        "{}/bin/".format(version),
        "stow/gcc-{}/bin/".format(version),
        "stow/llvm-{}/bin/".format(version),
        "stow/gcc-{}/*-mongodb-linux/bin/".format(version),
    ]
    return native.glob([
        directory + tool + suffix
        for directory in dirs
        for tool in tools
        for suffix in ["", ".*"]
    ])

def mongo_toolchain_filegroups(version, support_files = [], host_runtime_files = []):
    """Declare tool inputs without LLVM development archives, docs, or other tools.

    Args:
        version: MongoDB toolchain version (e.g. v5).
        support_files: Profile data, sanitizer denylists, and sysroot inputs.
        host_runtime_files: Native libraries for composed cross output wrappers.
    """

    # Runtime shared libraries are deliberately selected by extension rather
    # than a DT_NEEDED allowlist: dependencies vary across distros/architectures.
    # Keep them in both layouts to cover the binaries' relative RPATHs.
    native.filegroup(
        name = "tool_runtime_files",
        srcs = _archive_files([
            "{}/lib/*.so*".format(version),
            "{}/lib64/*.so*".format(version),
            "{}/lib/bfd-plugins/*.so*".format(version),
            "stow/*/lib/*.so*",
            "stow/*/lib64/*.so*",
            "stow/*/lib/bfd-plugins/*.so*",
            "stow/gcc-{}/libexec/gcc/*/*/liblto_plugin.so*".format(version),
            "openssl/lib*/*.so*",
        ]),
    )

    target_roots = ["", "target/"]

    # Bazel globs have no character classes; spell out the numeric prefixes.
    drivers = [
        "clang",
        "clang++",
        "*gcc*",
        "*g++",
        "*c++",
        "cpp",
        "as",
        "*-as",
    ] + ["clang-{}*".format(digit) for digit in range(10)]
    gcc_helpers = _archive_files([
        "stow/gcc-{}/libexec/**/*".format(version),
    ])
    driver_configs = _archive_files([
        "{}/clang*.cfg".format(version),
        "stow/llvm-{}/clang*.cfg".format(version),
    ])
    driver_files = [":tool_runtime_files"] + support_files + gcc_helpers + driver_configs
    compiler_tools = _tools(version, drivers + ["gcov", "llvm-cov", "llvm-profdata"])
    linker_tools = _tools(version, drivers + ["ld", "ld.bfd", "ld.gold", "ld.lld", "lld"])

    native.filegroup(
        name = "compiler_files",
        srcs = driver_files + compiler_tools + _archive_files([
            "{}/include/**/*".format(version),
            "stow/gcc-{}/include/**/*".format(version),
            "stow/libedit-{}/include/**/*".format(version),
            "stow/openssl*/include/**/*",
            "openssl/include/**/*",
            "stow/gcc-{}/lib/gcc/**/include*/**/*".format(version),
            "stow/llvm-{}/lib/clang/*/include/**/*".format(version),
            "stow/llvm-{}/lib/clang/*/share/**/*".format(version),
            "{}/lib/clang/*/include/**/*".format(version),
            "{}/lib/clang/*/share/**/*".format(version),
        ], roots = target_roots),
    )

    # Link actions need drivers, GCC's collect2/LTO helpers, startup objects,
    # GCC specs and compiler/sanitizer runtimes, but no C++ or LLVM SDK headers.
    native.filegroup(
        name = "linker_tool_files",
        srcs = driver_files + linker_tools + _archive_files([
            "{}/lib/*.o".format(version),
            "{}/lib64/*.o".format(version),
            "{}/lib/*.so*".format(version),
            "{}/lib64/*.so*".format(version),
            "stow/gcc-{}/lib/*.a".format(version),
            "stow/gcc-{}/lib64/*.a".format(version),
            "stow/openssl*/lib*/*.a",
            "openssl/lib*/*.a",
            "stow/gcc-{}/lib/*.so*".format(version),
            "stow/gcc-{}/lib64/*.so*".format(version),
            "stow/gcc-{}/lib/gcc/**/*.a".format(version),
            "stow/gcc-{}/lib/gcc/**/*.o".format(version),
            "stow/gcc-{}/lib/gcc/**/*.so*".format(version),
            "stow/gcc-{}/lib/gcc/**/specs".format(version),
            "stow/llvm-{}/lib/clang/*/lib/**/*".format(version),
            "stow/llvm-{}/lib/clang/*/share/**/*".format(version),
            "{}/lib/clang/*/lib/**/*".format(version),
            "{}/lib/clang/*/share/**/*".format(version),
            "mold-*/bin/*",
            "mold-*/lib/**/*",
            "mold-*/libexec/**/*",
        ] + [
            "{}/{}/{}*.a".format(version, libdir, library)
            for libdir in ["lib", "lib64"]
            for library in [
                "libgcc",
                "libstdc++",
                "libsupc++",
                "libatomic",
                "libasan",
                "libubsan",
                "libtsan",
                "libmsan",
                "liblsan",
                "libhwasan",
                "libssp",
                "libgomp",
                "libitm",
                "libquadmath",
                "libcrypto",
                "libssl",
                "libclang_rt",
            ]
        ], roots = target_roots),
    )

    for name, tools in [
        ("ar_files", ["ar", "llvm-ar", "ranlib", "llvm-ranlib"]),
        ("dwp_files", ["dwp", "llvm-dwp"]),
        ("objcopy_files", ["objcopy", "llvm-objcopy"]),
        # llvm-strip is an alias of llvm-objcopy in the LLVM installation.
        ("strip_files", ["strip", "llvm-strip", "llvm-objcopy"]),
    ]:
        native.filegroup(
            name = name,
            srcs = [":tool_runtime_files"] + _tools(version, tools),
        )
        native.filegroup(
            name = "host_" + name,
            srcs = [":tool_runtime_files"] + host_runtime_files + _tools(version, tools, host = True),
        )
