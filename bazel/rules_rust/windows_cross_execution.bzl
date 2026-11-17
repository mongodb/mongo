"""Keep Windows Rust tools local when cross-build actions use Linux RBE."""

def windows_cross_execution_requirements(enabled, exec_os):
    # Use the compiler's execution OS, not the crate's target OS: proc macros
    # and bootstrap tools in a Windows build can themselves be Linux targets.
    return {"no-remote-exec": "1"} if enabled and exec_os == "windows" else {}

def windows_cross_action_environment(enabled, exec_os, env):
    """Use a Linux PATH for remote Rust tools while preserving native actions."""
    result = dict(env)
    path = result.get("PATH", "")
    if enabled and exec_os == "linux" and (not path or ";" in path or "\\" in path or path[1:2] == ":"):
        # The Windows Bazel client's default shell environment is unsuitable
        # for /usr/bin/env bash and other tools inside the Linux action image.
        result["PATH"] = "/usr/local/bin:/usr/bin:/bin"
    return result

def windows_cross_cargo_environment(enabled, exec_os, env):
    result = windows_cross_action_environment(enabled, exec_os, env)
    if enabled and exec_os == "linux":
        # Failed build scripts also upload their declared manifest directory.
        # Linux absolute symlinks in that output cannot be downloaded on Windows.
        result["RULES_RUST_CARGO_MANIFEST_COPY"] = "1"

        # Build scripts run in CARGO_MANIFEST_DIR; expose execroot-relative
        # toolchain flags, INCLUDE/LIB, and paths inside the Clang VFS overlay.
        result["RULES_RUST_SYMLINK_EXEC_ROOT"] = "1"
    return result
