"""Custom signing macros for test extensions."""

load("//bazel:mongo_src_rules.bzl", "mongo_cc_extension_shared_library")

def _gpg_sign_impl(ctx):
    outs = []

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime

    for src in ctx.files.srcs:
        out = ctx.actions.declare_file(src.basename + ".sig")
        outs.append(out)

        # Inputs to this action
        inputs = [src, ctx.file.key, ctx.file._gpg_signer]
        pass_arg = ""
        if ctx.file.passphrase:
            inputs.append(ctx.file.passphrase)
            pass_arg = ctx.file.passphrase.path

        dep_files = ctx.files.gpg_bins
        dep_dirs = [f.dirname for f in dep_files]

        # Prefer explicit override (if provided). Otherwise, locate the real gpg binary from the bundle.
        gpg_file = ctx.file.gpg_main_script
        if gpg_file == None:
            for f in dep_files:
                if f.basename == "gpg" or f.basename == "gpg.exe":
                    gpg_file = f
                    break
        if gpg_file == None:
            fail("Unable to find gpg in gpg_bins. Ensure @gpg//:gpg_bins contains a 'gpg' binary.")

        env = dict(ctx.configuration.default_shell_env)
        sep = ctx.configuration.host_path_separator  # ":" or ";"
        base_path = env.get("PATH", "")
        env["PATH"] = sep.join(dep_dirs + ([base_path] if base_path else []))

        inputs += dep_files

        # Needed for remote execution: gpg binaries use RUNPATH=$ORIGIN/../libs.
        inputs += ctx.files.gpg_libs

        inputs += python.files.to_list()

        # Run the signer via the hermetic python toolchain (no shell involved)
        ctx.actions.run(
            inputs = inputs,
            outputs = [out],
            tools = [],
            executable = python.interpreter.path,
            env = env,
            arguments = [
                ctx.file._gpg_signer.path,
                gpg_file.path,  # $1
                ctx.file.key.path,  # $2
                pass_arg,  # $3 (empty if none)
                out.path,  # $4
                src.path,  # $5,
            ],
            progress_message = "Signing {}".format(src.basename),
            mnemonic = "GpgSign",
        )

    return [
        DefaultInfo(files = depset(outs)),
        OutputGroupInfo(signatures = depset(outs), originals = depset(ctx.files.srcs)),
    ]

gpg_sign = rule(
    implementation = _gpg_sign_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "key": attr.label(allow_single_file = True, mandatory = True),
        "passphrase": attr.label(allow_single_file = True),
        "gpg_main_script": attr.label(
            allow_single_file = True,
            doc = "Optional override for the gpg binary path. If unset, uses @gpg//:gpg_bins to locate 'gpg'.",
        ),
        "gpg_bins": attr.label(default = Label("@gpg//:gpg_bins")),
        "gpg_libs": attr.label(default = Label("@gpg//:gpg_libs")),
        "_gpg_signer": attr.label(
            allow_single_file = True,
            default = Label("//bazel:gpg_signer.py"),
        ),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    fragments = ["py"],
)

# Extensions must be signed in order to be loaded into the server. This macros allows users to build
# the extension shared object, and sign it with the provided PGP key. The target for the packaged
# output is name + "_signed_lib". It's necessary to reference this target as a dependency to ensure
# the signed artifacts are generated.
def signed_mongo_cc_extension_shared_library(
        name,
        srcs = [],
        deps = [],
        private_hdrs = [],
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = [],
        exec_properties = {},
        # signing
        # path to the GPG key, we are currently going to get this from the mongo repo
        key = "//src/mongo/db/extension/test_examples/test_extensions_signing_keys:test_extensions_signing_private_key.asc",
        passphrase = None,
        sign_visibility = None,
        sign_tags = None,
        **kwargs):
    """Build an extension shared library and sign it with a temporary GPG homedir.

    Args:
      name: Name of the extension shared library target.
      srcs: Sources for the extension library.
      deps: Dependencies for the extension library.
      private_hdrs: Private headers for the extension library.
      visibility: Visibility for created targets.
      data: Runtime data deps for the extension library.
      tags: Tags for created targets.
      copts: C/C++ compile options for the extension library.
      linkopts: Link options for the extension library.
      includes: Include paths for the extension library.
      linkstatic: Whether to link statically (see underlying rule semantics).
      local_defines: Local preprocessor defines for the extension library.
      target_compatible_with: Platform constraints for created targets.
      defines: Preprocessor defines for the extension library.
      additional_linker_inputs: Extra linker inputs for the extension library.
      features: Bazel features to enable/disable on the extension library.
      exec_properties: Exec properties for created actions.
      key: Label of the signing key file (private key).
      passphrase: Optional label of a file containing the signing key passphrase.
      sign_visibility: Optional visibility override for signing targets.
      sign_tags: Optional tags for signing targets.
      **kwargs: Forwarded to the underlying extension rule.
    """
    if key == None:
        fail("signed_mongo_cc_extension_shared_library requires a pgp key")

    sig_name = name + "_sig"

    # 1) Build the shared object
    mongo_cc_extension_shared_library(
        name = name,
        srcs = srcs,
        deps = deps,
        private_hdrs = private_hdrs,
        visibility = visibility,
        data = data,
        tags = tags,
        copts = copts,
        linkopts = linkopts,
        includes = includes,
        linkstatic = linkstatic,
        local_defines = local_defines,
        target_compatible_with = target_compatible_with,
        defines = defines,
        additional_linker_inputs = additional_linker_inputs,
        features = features,
        exec_properties = exec_properties,
        **kwargs
    )

    # 2) Sign the produced library (ctx.files.srcs for a rule label includes its default outputs)
    gpg_sign(
        name = sig_name,
        srcs = [":" + name],
        key = key,
        passphrase = passphrase,
        visibility = sign_visibility if sign_visibility != None else visibility,
        tags = (sign_tags if sign_tags != None else []),
    )

    # 3) Aggregate both files under name + "_signed_lib" for consumption
    signed_bundle_name = name + "_signed_lib"

    native.filegroup(
        name = signed_bundle_name,
        srcs = [":" + name, ":" + sig_name],
        visibility = visibility,
        tags = tags,
    )
