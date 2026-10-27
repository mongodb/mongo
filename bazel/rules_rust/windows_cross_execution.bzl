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

def windows_cross_native_cc_environment(enabled, exec_os, env):
    """Use the native MSVC toolchain's absolute paths without Bazel's execpath prefix."""
    result = dict(env)
    if enabled and exec_os == "windows":
        # The native MSVC toolchain records absolute paths under Program Files.
        # Bazel's execpath representation of those paths is not a valid Windows
        # filename for cc-rs. Strip only the prefix, retaining the selected tool
        # and version rather than searching PATH or the working directory.
        for key in ["CC", "CXX", "AR", "LD"]:
            path = result[key].replace("\\", "/")
            drive_index = path.find(":/")
            if drive_index >= 1 and path[drive_index - 1].lower() in "abcdefghijklmnopqrstuvwxyz":
                path = path[drive_index - 1:]
            elif key != "LD" and not path.startswith("//"):
                fail("Expected an absolute native Windows toolchain path for {}: {}".format(key, path))

            # With linker_preference=rust, LD is a declared execroot-relative
            # Rust linker. The Cargo runner already resolves it against execroot.
            result[key] = path
    return result
