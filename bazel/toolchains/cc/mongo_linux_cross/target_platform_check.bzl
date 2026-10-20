"""Check the cross target platform and execute build-time Python on ARM64."""

def _s390x_target_platform_check_impl(ctx):
    if not ctx.target_platform_has_constraint(ctx.attr._s390x[platform_common.ConstraintValueInfo]):
        fail("The cross smoke build requires an s390x target platform")
    python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
    output = ctx.actions.declare_file(ctx.label.name + "_python.txt")
    ctx.actions.run(
        executable = python.interpreter.path,
        arguments = [
            "-c",
            "import pathlib, platform, sys; machine = platform.machine(); assert machine in ('aarch64', 'arm64'), machine; pathlib.Path(sys.argv[1]).write_text(machine + '\\n' + sys.version + '\\n')",
            output.path,
        ],
        inputs = python.files,
        outputs = [output],
        mnemonic = "PythonCrossToolchainSmoke",
        progress_message = "Verify ARM64 build-time Python for the s390x cross target",
    )
    return [DefaultInfo(files = depset([output]))]

s390x_target_platform_check = rule(
    implementation = _s390x_target_platform_check_impl,
    attrs = {"_s390x": attr.label(default = "@platforms//cpu:s390x")},
    toolchains = ["@rules_python//python:toolchain_type"],
)

def _ppc64le_target_platform_check_impl(ctx):
    if not ctx.target_platform_has_constraint(ctx.attr._ppc64le[platform_common.ConstraintValueInfo]):
        fail("The cross smoke build requires a ppc64le target platform")
    return []

ppc64le_target_platform_check = rule(
    implementation = _ppc64le_target_platform_check_impl,
    attrs = {"_ppc64le": attr.label(default = "@platforms//cpu:ppc")},
)
