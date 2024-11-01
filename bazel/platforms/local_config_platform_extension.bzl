def _translate_cpu(arch):
    if arch in ["i386", "i486", "i586", "i686", "i786", "x86"]:
        return "x86_32"
    if arch in ["amd64", "x86_64", "x64"]:
        return "x86_64"
    if arch in ["ppc", "ppc64", "ppc64le"]:
        return "ppc"
    if arch in ["arm", "armv7l"]:
        return "arm"
    if arch in ["aarch64"]:
        return "aarch64"
    if arch in ["s390x", "s390"]:
        return "s390x"
    if arch in ["mips64el", "mips64"]:
        return "mips64"
    if arch in ["riscv64"]:
        return "riscv64"
    return None

def _translate_os(os):
    if os.startswith("mac os"):
        return "osx"
    if os.startswith("freebsd"):
        return "freebsd"
    if os.startswith("openbsd"):
        return "openbsd"
    if os.startswith("linux"):
        return "linux"
    if os.startswith("windows"):
        return "windows"
    return None

def _host_platform_repo_impl(rctx):
    cpu = _translate_cpu(rctx.os.arch)
    os = _translate_os(rctx.os.name)

    cpu = "" if cpu == None else "  '@platforms//cpu:%s',\n" % cpu
    os = "" if os == None else "  '@platforms//os:%s',\n" % os

    rctx.file("BUILD.bazel", """
# DO NOT EDIT: automatically generated BUILD file
exports_files(["constraints.bzl"])
""")

    rctx.file("constraints.bzl", """
# DO NOT EDIT: automatically generated constraints list
HOST_CONSTRAINTS = [
%s%s]
""" % (cpu, os))

host_platform_repo = repository_rule(
    implementation = _host_platform_repo_impl,
    doc = """Generates constraints for the host platform. The constraints.bzl
file contains a single <code>HOST_CONSTRAINTS</code> variable, which is a
list of strings, each of which is a label to a <code>constraint_value</code>
for the host platform.""",
)

def _host_platform_impl(_mctx):
    host_platform_repo(name = "host_platform")

host_platform = module_extension(
    implementation = _host_platform_impl,
    doc = """Generates a <code>host_platform_repo</code> repo named
<code>host_platform</code>, containing constraints for the host platform.""",
)
