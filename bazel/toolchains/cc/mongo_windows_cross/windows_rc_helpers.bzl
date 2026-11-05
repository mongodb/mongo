"""Helpers for resource compilation with the Windows cross toolchain."""

_WINDOWS_CROSS_RC_INCLUDE_DIRS = [
    "msvc/include",
    "msvc/atlmfc/include",
    "winsdk/include/ucrt",
    "winsdk/include/shared",
    "winsdk/include/um",
    "winsdk/include/winrt",
    "winsdk/include/cppwinrt",
]

def windows_cross_rc_include_dirs(ctx):
    """Returns Windows cross sysroot include directories from staged files."""
    if not ctx.files.rc_sysroot:
        return []

    sysroot = None
    separator = "/"
    for file in ctx.files.rc_sysroot:
        if "/sysroot/" in file.path:
            sysroot = file.path.split("/sysroot/", 1)[0] + "/sysroot"
            break
        if "\\sysroot\\" in file.path:
            sysroot = file.path.split("\\sysroot\\", 1)[0] + "\\sysroot"
            separator = "\\"
            break

    if sysroot == None:
        fail("windows_rc: rc_sysroot files are not under a sysroot directory")

    return [
        sysroot + separator + relative.replace("/", separator)
        for relative in _WINDOWS_CROSS_RC_INCLUDE_DIRS
    ]

def windows_cross_rc_source(ctx):
    """Creates an RC source with paths to Bazel's staged resource inputs."""
    substitutions = {}
    for resource in ctx.files.resources:
        short_path = resource.short_path.replace("\\", "/")
        if short_path.startswith("src/"):
            logical_path = short_path[len("src/"):]
        else:
            path_parts = short_path.split("/src/")
            if len(path_parts) == 1:
                continue
            logical_path = path_parts[-1]

        # Headers are found through the include search path. Rewriting an
        # #include to a path relative to the generated RC source makes Clang
        # look below the RC output directory and fail to find the header.
        if logical_path.endswith(".h"):
            continue

        substitutions[logical_path] = resource.path.replace("\\", "/")

    if not substitutions:
        return ctx.file.src

    rc_source = ctx.actions.declare_file(ctx.label.name + "_windows_cross.rc")
    ctx.actions.expand_template(
        template = ctx.file.src,
        output = rc_source,
        substitutions = substitutions,
    )
    return rc_source

def windows_cross_rc_library_dirs(files):
    """Returns loader paths for the bundled shared libraries staged with RC tools."""
    runtime_dirs = {}
    archive_dirs = {}
    for file in files:
        if not (file.basename.endswith(".so") or ".so." in file.basename):
            continue
        directory = file.dirname.replace("\\", "/")

        # Windows archive extraction can leave placeholder symlinks in llvm/lib.
        # Prefer the regular library copies prepared by the toolchain repository.
        if directory.endswith("/runtime_libs"):
            runtime_dirs[directory] = True
        else:
            archive_dirs[directory] = True
    return sorted(runtime_dirs.keys()) + sorted(archive_dirs.keys())
