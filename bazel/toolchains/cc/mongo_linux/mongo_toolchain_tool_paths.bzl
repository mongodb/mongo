"""Single source of truth for mongo_linux toolchain tool paths.

The generated toolchain BUILD file consumes these mappings for both local and remote
execution. Local actions are routed into the persistent container by Bazel's spawn
strategy, so the toolchain itself does not need wrapper stubs.
"""

def get_mongo_toolchain_tool_paths(version, compiler):
    """Returns the tool_paths mapping for a mongo_linux toolchain configuration.

    Args:
        version: toolchain version string, e.g. "v5".
        compiler: "gcc" or "clang".

    Returns:
        dict of cc_toolchain_config tool name to repo-relative (or absolute) tool path.
    """

    # Note: You might assume that the specification of `compiler_name` would be
    # sufficient to make Bazel use the correct binary. This is incorrect; Bazel appears
    # to unconditionally use the `gcc` tool_path. As a result, we have to conditionally
    # set the value pointed to by `gcc`.
    if compiler == "gcc":
        return {
            "gcc": version + "/bin/gcc",
            "g++": version + "/bin/g++",
            "cpp": version + "/bin/cpp",
            "ar": version + "/bin/ar",
            "nm": version + "/bin/llvm-nm",
            "ld": version + "/bin/ld",
            "as": version + "/bin/as",
            "dwp": version + "/bin/dwp",
            "objcopy": version + "/bin/llvm-objcopy",
            "objdump": version + "/bin/objdump",
            "strip": version + "/bin/strip",
            "gcov": version + "/bin/gcov",
            "llvm-cov": "/bin/false",  # /bin/false = we're not using llvm-cov
        }
    if compiler == "clang":
        # TODO(SERVER-87211): The `gcc` and `g++` entries below are using paths that help
        # clang find the sanitizer .a files. Switch these to the {version}/bin/* paths
        # once EngFlow fixes the issue where symlinks are fully resolved when copied to
        # the remote execution system.
        return {
            "gcc": version + "/bin/clang",
            "g++": "stow/llvm-" + version + "/bin/clang++",
            "cpp": version + "/bin/cpp",
            "ar": version + "/bin/ar",
            "nm": version + "/bin/llvm-nm",
            "ld": version + "/bin/ld",
            "as": version + "/bin/as",
            "dwp": version + "/bin/llvm-dwp",
            "objcopy": version + "/bin/llvm-objcopy",
            "objdump": version + "/bin/objdump",
            "strip": version + "/bin/strip",
            "gcov": version + "/bin/llvm-profdata",
            "llvm-cov": version + "/bin/llvm-cov",
        }
    fail("Unknown mongo_linux toolchain compiler: " + compiler)
