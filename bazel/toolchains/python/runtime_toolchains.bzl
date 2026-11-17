"""Register Python runtimes for programs and build tools."""

def python_runtime_toolchains(name, runtime_pair, constraints, windows = False):
    """Expose the target runtime and, on Windows, a native build-tool runtime.

    Args:
        name: Name of the target runtime toolchain.
        runtime_pair: Label of the Python runtime pair.
        constraints: OS and CPU constraints of the runtime.
        windows: Whether the runtime supports Windows target and execution modes.
    """
    if windows:
        # WASI targets still run build tools on the Windows host. Register this
        # entry before the target runtime so toolchain resolution keeps those
        # actions native instead of selecting a Linux execution platform.
        native.toolchain(
            name = name + "_exec",
            exec_compatible_with = constraints,
            toolchain = runtime_pair,
            toolchain_type = "@rules_python//python:toolchain_type",
            visibility = ["//visibility:public"],
        )

    # A Windows program needs Windows Python even when its build actions run
    # on Linux. Other runtimes keep their execution-platform compatibility.
    native.toolchain(
        name = name,
        target_compatible_with = constraints if windows else [],
        exec_compatible_with = [] if windows else constraints,
        toolchain = runtime_pair,
        toolchain_type = "@rules_python//python:toolchain_type",
        visibility = ["//visibility:public"],
    )
