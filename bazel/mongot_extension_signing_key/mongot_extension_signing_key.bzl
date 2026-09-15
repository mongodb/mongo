"""Rules for downloading and embedding mongot_extension_signing_key"""

load("@internal_platforms_do_not_use//host:constraints.bzl", "HOST_CONSTRAINTS")

def _gpg_export_armored_key_impl(ctx):
    key = ctx.file.key
    armored_key_output_file = ctx.outputs.armored_key_output_file
    host_python = ctx.exec_groups["host"].toolchains["@rules_python//python:toolchain_type"].py3_runtime

    # Inputs to this action
    inputs = [key, ctx.file.script] + host_python.files.to_list()

    pass_file = ""
    if ctx.file.passphrase:
        inputs.append(ctx.file.passphrase)
        pass_file = ctx.file.passphrase.path

    # Collect tool files from the filegroups
    dep_files = ctx.files.gpg_bins
    dep_dirs = [f.dirname for f in dep_files]

    # Find the gpg executable
    gpg_bin = None
    for f in dep_files:
        if f.basename == "gpg" or f.basename == "gpg.exe":
            gpg_bin = f
            break
    if gpg_bin == None:
        fail("Unable to find gpg in gpg_bins. Ensure @gpg//:gpg_bins contains a 'gpg' binary.")

    env = dict(ctx.configuration.default_shell_env)
    sep = ctx.configuration.host_path_separator  # ":" or ";"
    base_path = env.get("PATH", "")
    env["PATH"] = sep.join(dep_dirs + ([base_path] if base_path else []))

    inputs += dep_files

    # Needed for remote execution: gpg binaries use RUNPATH=$ORIGIN/../libs.
    inputs += ctx.files.gpg_libs

    # Arguments your Python helper expects: <gpg> <key> <passphrase_or_empty> <armored_key_output_file>
    ctx.actions.run(
        executable = host_python.interpreter.path,
        arguments = [
            ctx.file.script.path,
            gpg_bin.path,
            key.path,
            pass_file,
            armored_key_output_file.path,
        ],
        inputs = inputs,
        tools = [],
        outputs = [armored_key_output_file],
        env = env,
        exec_group = "host",
        mnemonic = "GpgExportArmored",
        progress_message = "Export armored key to %s" % armored_key_output_file.path,
    )

gpg_export_armored_key = rule(
    implementation = _gpg_export_armored_key_impl,
    attrs = {
        "key": attr.label(allow_single_file = True, mandatory = True),
        "passphrase": attr.label(allow_single_file = True),
        "armored_key_output_file": attr.output(mandatory = True),
        "script": attr.label(
            default = Label("//bazel/mongot_extension_signing_key:gpg_export_armored_key.py"),
            allow_single_file = True,
        ),
        "gpg_bins": attr.label(
            default = Label("@gpg//:gpg_bins"),
        ),
        "gpg_libs": attr.label(
            default = Label("@gpg//:gpg_libs"),
        ),
    },
    exec_groups = {
        "host": exec_group(
            exec_compatible_with = HOST_CONSTRAINTS,
            toolchains = ["@rules_python//python:toolchain_type"],
        ),
    },
)

def _generate_embedded_public_key_header_impl(ctx):
    script = ctx.file.script
    public_key_path = ctx.file.public_key_path
    embedded_key_header_path = ctx.outputs.embedded_key_header_path
    host_python = ctx.exec_groups["host"].toolchains["@rules_python//python:toolchain_type"].py3_runtime
    inputs = host_python.files.to_list() + [script, public_key_path]

    ctx.actions.run(
        executable = host_python.interpreter.path,
        arguments = [
            script.path,
            "--public_key_path",
            public_key_path.path,
            "--embedded_key_header_path",
            embedded_key_header_path.path,
        ],
        inputs = inputs,
        tools = [],
        outputs = [embedded_key_header_path],
        exec_group = "host",
        mnemonic = "EmbedPublicKeyHeader",
        progress_message = "Generate embedded key to %s" % embedded_key_header_path.path,
    )

generate_embedded_public_key_header = rule(
    implementation = _generate_embedded_public_key_header_impl,
    attrs = {
        "public_key_path": attr.label(allow_single_file = True, mandatory = True),
        "embedded_key_header_path": attr.output(mandatory = True),
        "script": attr.label(
            default = Label("//bazel/mongot_extension_signing_key:generate_embedded_public_key_header.py"),
            allow_single_file = True,
        ),
    },
    exec_groups = {
        "host": exec_group(
            exec_compatible_with = HOST_CONSTRAINTS,
            toolchains = ["@rules_python//python:toolchain_type"],
        ),
    },
)
