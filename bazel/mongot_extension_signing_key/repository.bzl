"""Repository rule for the mongot extension signing public key.

This file is kept separate from the analysis-time signing rules because the
repository rule is evaluated while MODULE.bazel is being interpreted. Loading
the generated host-platform constraints from that same file would create a
module-extension cycle.
"""

def _impl(ctx):
    ctx.download(
        url = "https://pgp.mongodb.com/mongot-extension.pub",
        sha256 = "2a15e6a2d9f6c0d8141dad515d9360f6cf01e1a11f7e2c3bc0820e18c5e9d0b7",
        output = "mongot-extension.pub",
    )
    ctx.file("BUILD.bazel", 'exports_files(["mongot-extension.pub"])')

mongot_extension_signing_key_repo = repository_rule(implementation = _impl)
