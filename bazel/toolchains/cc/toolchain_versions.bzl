"""The production toolchain policy is shared with the package tests."""

def production_llvm_version(ctx, platform):
    """Read the LLVM archive version from the production platform map."""
    versions = json.decode(ctx.read(Label("//buildscripts/package_test:toolchain_versions.json")))
    return versions[platform]["compiler_version"]
