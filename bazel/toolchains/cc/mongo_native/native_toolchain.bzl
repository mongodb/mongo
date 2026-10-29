"""Repository rule that builds a MongoDB cc toolchain around a native/third-party
system compiler (clang or gcc) instead of the bundled hermetic mongo toolchain.
"""

def _which(ctx, name):
    if name.startswith("/"):
        return name
    path = ctx.which(name)
    if path == None:
        fail("Could not find '{}' on PATH; set CC/CXX to absolute paths.".format(name))
    return str(path)

def _detect_builtin_include_dirs(ctx, compiler, lang_flag):
    # Ask the compiler for its builtin header search paths. They are printed to
    # stderr between the "#include <...> search starts here:" and
    # "End of search list." markers.
    result = ctx.execute([compiler, "-E", lang_flag, "/dev/null", "-v"])
    if result.return_code != 0:
        fail("Failed to probe include dirs via '{}': {}".format(compiler, result.stderr))
    dirs = []
    collecting = False
    for line in result.stderr.splitlines():
        if "search starts here:" in line:
            collecting = True
            continue
        if "End of search list." in line:
            collecting = False
            continue
        if collecting:
            entry = line.strip()

            # clang annotates framework dirs with "(framework directory)".
            entry = entry.replace(" (framework directory)", "")
            if entry and entry not in dirs:
                dirs.append(entry)
    return dirs

def _setup_mongo_native_toolchain(ctx):
    arch = ctx.os.arch
    bazel_cpu = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "aarch64": "aarch64",
        "arm64": "aarch64",
        "ppc64le": "ppc",
        "s390x": "s390x",
    }.get(arch, arch)

    def _fmt_list(items):
        return "".join(['\n        "%s",' % i for i in items])

    if ctx.os.environ.get("USE_NATIVE_TOOLCHAIN") != "1":
        ctx.template(
            "BUILD.bazel",
            ctx.attr.build_tpl,
            substitutions = {
                "{cc}": "cc",
                "{cxx}": "c++",
                "{bazel_cpu}": bazel_cpu,
                "{all_include_dirs}": "",
                "{c_include_dirs}": "",
                "{cpp_only_include_dirs}": "",
            },
        )
        return

    cc = _which(ctx, ctx.os.environ.get("CC", "cc"))
    cxx = _which(ctx, ctx.os.environ.get("CXX", "c++"))

    c_dirs = _detect_builtin_include_dirs(ctx, cc, "-xc")
    cxx_dirs = _detect_builtin_include_dirs(ctx, cxx, "-xc++")
    cpp_only_dirs = [d for d in cxx_dirs if d not in c_dirs]

    # C++ compiles need the C++-only dirs searched first (correct #include_next
    # order), then the shared C dirs.
    all_dirs = cpp_only_dirs + [d for d in c_dirs if d not in cpp_only_dirs]

    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = {
            "{cc}": cc,
            "{cxx}": cxx,
            "{bazel_cpu}": bazel_cpu,
            "{all_include_dirs}": _fmt_list(all_dirs),
            "{c_include_dirs}": _fmt_list(c_dirs),
            "{cpp_only_include_dirs}": _fmt_list(cpp_only_dirs),
        },
    )

setup_mongo_native_toolchain = repository_rule(
    implementation = _setup_mongo_native_toolchain,
    attrs = {
        "build_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_native:mongo_native_toolchain.BUILD.tmpl",
        ),
    },
    environ = ["CC", "CXX", "USE_NATIVE_TOOLCHAIN"],
    configure = True,
)
