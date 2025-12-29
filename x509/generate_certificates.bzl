load("@poetry//:dependencies.bzl", "dependency")
load("//bazel/config:render_template.bzl", "render_template")

def _generate_certificates(ctx):
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_libs = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.py_libs]

    python_path = []
    for py_dep in ctx.attr.py_libs:
        for path in py_dep[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(ctx.expand_make_variables("python_library_imports", "$(BINDIR)/external/" + path, ctx.var))

    # Write the cert definitions to a temporary file, which mkcert.py will read from.
    certfile = ctx.actions.declare_file("." + ctx.label.name + ".certs.json")
    ctx.actions.write(
        output = certfile,
        content = ctx.attr.certs_def,
    )
    certs_def = json.decode(ctx.attr.certs_def)
    out_set = {}

    # Statically compute output files from cert definitions
    for cert in certs_def["certs"]:
        # Each cert def generates cert.name and a SHA-256 and SHA-1 digest of cert.name.
        out_set[cert["name"]] = None
        out_set[cert["name"] + ".digest.sha256"] = None
        out_set[cert["name"] + ".digest.sha1"] = None
        if cert.get("split_cert_and_key", False):
            # Split certs additionally generate .crt and .key files.
            crt_name = cert["name"][:-len(".pem")] + ".crt"
            key_name = cert["name"][:-len(".pem")] + ".key"
            out_set[crt_name] = None
            out_set[key_name] = None
        if cert.get("pkcs12", None) != None:
            # PKCS12 certs generate a separate cert bundle at cert.pkcs12.name.
            pkcs12_name = cert["pkcs12"].get("name", cert["name"])
            out_set[pkcs12_name] = None
    should_gen_crls = False
    if "crls" in certs_def:
        for crl in certs_def["crls"]:
            # Each CRL def generates crl and the two digests.
            out_set[crl] = None
            out_set[crl + ".digest.sha256"] = None
            out_set[crl + ".digest.sha1"] = None
            should_gen_crls = True

    outputs = [ctx.actions.declare_file(out) for out in out_set]

    # Run the Python script to generate the certificates, sending stdout to /dev/null to avoid
    # cluttering the build log.
    args = ctx.actions.args()
    args.add(ctx.expand_location(ctx.attr.main))
    args.add(certfile.path)
    args.add("--mkcrl" if should_gen_crls else "--no-mkcrl")
    args.add("--quiet")
    args.add("--output", outputs[0].dirname)

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = outputs,
        inputs = depset(
            direct = [certfile] + ctx.files.static_inputs,
            transitive = [python.files, depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])] + python_libs,
        ),
        arguments = [args],
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
        mnemonic = "CertificateGenerator",
    )

    return [DefaultInfo(files = depset(outputs))]

generate_certificates = rule(
    implementation = _generate_certificates,
    attrs = {
        "static_inputs": attr.label_list(mandatory = True, allow_files = True, doc = "Static input files required to generate certificates."),
        "certs_def": attr.string(mandatory = True, doc = "Definitions for all certificates."),
        "srcs": attr.label_list(
            doc = "The input files of this rule.",
            allow_files = True,
            default = [
                Label("//x509:mkcert.py"),
            ],
        ),
        "main": attr.string(
            doc = "The main Python file to execute.",
            default = "$(location //x509:mkcert.py)",
        ),
        "py_libs": attr.label_list(
            default = [
                dependency(
                    "ecdsa",
                    group = "testing",
                ),
                dependency(
                    "asn1crypto",
                    group = "testing",
                ),
                dependency(
                    "pyyaml",
                    group = "core",
                ),
                dependency(
                    "cryptography",
                    group = "platform",
                ),
            ],
        ),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
)
