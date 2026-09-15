"""Repository rule to extract a sysroot from the RBE container image.

When building locally on Linux (--config=local), the compiler and linker normally
use the host system's headers and libraries. This can cause mismatches vs. what
the RBE containers provide.

This rule pulls the RBE container image for the current distro using Docker,
exports its filesystem, and extracts it as a sysroot. The toolchain can then
use --sysroot to point the compiler/linker at this extracted filesystem,
ensuring local builds use the same system headers and libraries as RBE.

The dump is enabled for opted-in Amazon Linux 2023 native builds and for Linux
PPC64LE/s390x cross-RBE builds. Cross builds select the target architecture from
the multi-architecture pinned image rather than from the machine running Bazel.
"""

load("//bazel:utils.bzl", "get_host_distro_major_version", "retry_execute")
load("//bazel/platforms:remote_execution_containers.bzl", "REMOTE_EXECUTION_CONTAINERS")

SYSROOT_ENV_VAR = "USE_RBE_SYSROOT"
LINUX_CROSS_TOOLCHAIN_ENV_VAR = "MONGO_LINUX_CROSS_TOOLCHAIN"

# Repository rules execute before action wrappers, so forward the complete
# task-scoped Podman configuration and credential path rather than relying on
# the later container-action environment.
_CONTAINER_RUNTIME_ENV_VARS = [
    "CONTAINERS_STORAGE_CONF",
    "CONTAINERS_CONF",
    "REGISTRY_AUTH_FILE",
    "XDG_RUNTIME_DIR",
    "TMPDIR",
    "TMP",
    "TEMP",
]

_LINUX_CROSS_TARGET_ARCHES = ["ppc64le", "s390x"]
_LINUX_CROSS_TARGET_DISTROS = ["rhel8", "rhel9", "rhel10"]

def _linux_cross_sysroot_target(ctx):
    # This variable must be set via --repo_env (as the cross-RBE wrapper does),
    # never exported in a shell: it flips the native toolchain/sysroot repos
    # into cross mode for whoever runs Bazel in that environment.
    selector = ctx.os.environ.get(LINUX_CROSS_TOOLCHAIN_ENV_VAR, "")
    if not selector:
        return None

    target_and_exec = selector.split("_on_")
    if len(target_and_exec) != 2:
        return struct(error = "invalid {} value '{}'".format(LINUX_CROSS_TOOLCHAIN_ENV_VAR, selector))

    target = target_and_exec[0]
    for arch in _LINUX_CROSS_TARGET_ARCHES:
        arch_suffix = "_" + arch
        if target.endswith(arch_suffix):
            distro = target[:-len(arch_suffix)]
            if distro not in _LINUX_CROSS_TARGET_DISTROS:
                return struct(error = "unsupported Linux cross target distro '{}'".format(distro))
            return struct(arch = arch, distro = distro, error = None)

    return struct(error = "unsupported Linux cross target in '{}'".format(selector))

def sysroot_dump_disabled_reason(ctx):
    """Returns why the RBE sysroot dump is disabled on this host, or None.

    Shared between the sysroot_dump repository rule and the mongo toolchain
    repository rule so both make the same enable/disable decision at fetch
    time. When this returns a reason, the toolchain generates its pre-sysroot
    BUILD file and never references @rbe_sysroot, keeping the disabled path's
    analysis graph identical to what it was before sysroot support existed.

    Native sysroot dumping is only enabled on Amazon Linux 2023 hosts for now
    (including minor-version variants like amazon_linux_2023_3). This is
    because libs2n aggressively loads the highest openssl symbols available
    at build time, so binaries built against another distro's container
    sysroot break at runtime on older minor versions of that distro. Other
    distros keep using the host's system headers and libraries even if
    --//bazel/config:use_rbe_sysroot is set. Linux cross-RBE builds are handled
    separately: the pinned RHEL images are multi-architecture, so their target
    PPC64LE/s390x filesystem can be exported on an x86_64 or ARM64 Bazel host.
    """
    if ctx.os.name != "linux":
        return "only supported on Linux"

    cross_target = _linux_cross_sysroot_target(ctx)
    if cross_target != None:
        if cross_target.error != None:
            return cross_target.error
        if cross_target.distro not in REMOTE_EXECUTION_CONTAINERS:
            return "no RBE container found for distro '{}'".format(cross_target.distro)
        return None

    if ctx.os.arch in ("s390x", "ppc", "ppc64", "ppc64le"):
        return "native RBE sysroot dumping is not enabled for {}".format(ctx.os.arch)
    if ctx.os.environ.get(SYSROOT_ENV_VAR, "0") not in ["1", "true", "True", "yes"]:
        return "{} is not set".format(SYSROOT_ENV_VAR)
    distro = get_host_distro_major_version(ctx)
    if distro == None:
        return "could not detect host distro"
    if not distro.startswith("amazon_linux_2023"):
        return "only enabled on Amazon Linux 2023 hosts (host distro: {})".format(distro)
    if distro not in REMOTE_EXECUTION_CONTAINERS:
        return "no RBE container found for distro '{}'".format(distro)
    return None

def _container_runtime_env(ctx):
    """Returns explicitly forwarded runtime state for repository-rule CLIs."""
    return {
        name: ctx.os.environ[name]
        for name in _CONTAINER_RUNTIME_ENV_VARS
        if ctx.os.environ.get(name, "")
    }

def _platform_image_ref(ctx, docker, image_ref, target_arch, runtime_env):
    """Resolves a pinned image index to one platform-specific manifest."""
    if not target_arch:
        return image_ref

    result = retry_execute(
        ctx,
        tries = 5,
        arguments = [docker, "manifest", "inspect", image_ref],
        environment = runtime_env,
        timeout = 120,
    )
    if result.return_code != 0:
        fail("Failed to inspect RBE container image {}:\n{}".format(image_ref, result.stderr))

    manifest = json.decode(result.stdout)
    for image in manifest.get("manifests", []):
        platform = image.get("platform", {})
        if platform.get("os") == "linux" and platform.get("architecture") == target_arch:
            # Pull the child manifest under its own digest. Docker otherwise
            # cannot cache two architectures of the same index digest at once
            # and fails with "cannot overwrite digest" when switching targets.
            return image_ref.split("@")[0] + "@" + image["digest"]

    fail("RBE container image {} has no linux/{} manifest".format(image_ref, target_arch))

def _sysroot_dump_impl(ctx):
    """Extracts the RBE container filesystem to use as a sysroot for local builds."""

    # Make this rule re-run whenever the pinned container manifest changes.
    # The container URLs in remote_execution_containers.bzl are sha256-pinned,
    # so editing that file (e.g. bumping a digest) will invalidate the sysroot
    # and force re-extraction. Without this, Bazel would keep using the
    # previously extracted filesystem even after the upstream image is bumped.
    ctx.watch(ctx.path(Label("//bazel/platforms:remote_execution_containers.bzl")))

    disabled_reason = sysroot_dump_disabled_reason(ctx)
    if disabled_reason != None:
        # Only warn when the user explicitly asked for the sysroot; the
        # silent default path is the common case.
        if (
            ctx.os.environ.get(SYSROOT_ENV_VAR, "0") in ["1", "true", "True", "yes"] or
            ctx.os.environ.get(LINUX_CROSS_TOOLCHAIN_ENV_VAR, "")
        ):
            print("WARNING: Skipping RBE sysroot dump: {}.".format(disabled_reason))
        ctx.file("BUILD.bazel", _NOOP_BUILD)
        ctx.file("sysroot_info.bzl", 'SYSROOT_PATH = ""\nSYSROOT_ENABLED = False\nSYSROOT_CONTAINER_URL = ""\n')
        return

    cross_target = _linux_cross_sysroot_target(ctx)
    distro = cross_target.distro if cross_target != None else get_host_distro_major_version(ctx)
    target_arch = cross_target.arch if cross_target != None else None
    container_url = REMOTE_EXECUTION_CONTAINERS[distro]["container-url"]

    # Strip the "docker://" prefix to get the image reference.
    image_ref = container_url
    if image_ref.startswith("docker://"):
        image_ref = image_ref[len("docker://"):]

    # Look for a container CLI: prefer docker, fall back to podman (RHEL hosts
    # generally ship podman, not docker). Both share the same `pull`/`create`/
    # `export`/`rm` subcommands used below.
    docker = ctx.which("docker") or ctx.which("podman")
    if not docker:
        fail(
            "A container CLI (docker or podman) is required to extract the " +
            "RBE sysroot but neither was found on PATH.\n" +
            "Either install one or unset {}=1.".format(SYSROOT_ENV_VAR),
        )

    runtime_env = _container_runtime_env(ctx)
    storage_config = runtime_env.get("CONTAINERS_STORAGE_CONF", "")
    if storage_config and not ctx.path(storage_config).exists:
        fail(
            "CONTAINERS_STORAGE_CONF points to a missing Podman storage " +
            "configuration: {}".format(storage_config),
        )

    # Pulling the image is the network-dependent step, so give it the most
    # attempts. Pulls resume at the layer level, making retries cheap.
    platform_args = ["--platform", "linux/{}".format(target_arch)] if target_arch else []
    target_description = "{} {}".format(distro, target_arch) if target_arch else distro
    pull_ref = _platform_image_ref(ctx, docker, image_ref, target_arch, runtime_env)
    ctx.report_progress("Pulling RBE container image for {} sysroot".format(target_description))
    result = retry_execute(
        ctx,
        tries = 5,
        arguments = [docker, "pull"] + platform_args + [pull_ref],
        environment = runtime_env,
        timeout = 600,
    )
    if result.return_code != 0:
        fail("Failed to pull RBE container image {}:\n{}".format(pull_ref, result.stderr))

    # Create an unnamed container and capture its ID rather than using a fixed
    # name: a fixed name races with concurrent Bazel invocations on the same
    # host (e.g. a second checkout, or the compiledb output base) and the
    # cleanup `rm -f` could remove an unrelated container with the same name.
    ctx.report_progress("Creating temporary container to export filesystem")
    result = retry_execute(
        ctx,
        tries = 3,
        arguments = [docker, "create"] + platform_args + [pull_ref, "/bin/true"],
        environment = runtime_env,
        timeout = 60,
    )
    if result.return_code != 0:
        fail("Failed to create container for sysroot export:\n{}".format(result.stderr))
    container_id = result.stdout.strip()
    if not container_id:
        fail("docker create did not return a container ID for sysroot export.")

    ctx.report_progress("Exporting container filesystem (this may take a moment)")
    sysroot_dir = ctx.path("sysroot")

    # Export the container filesystem as a tar and extract only the directories
    # needed for a sysroot (headers + libraries). This avoids extracting device
    # nodes, lock files, and other OS artefacts that cause permission errors.
    # We use a bash script to try each path independently since not every
    # container has all of usr/lib64, lib64, etc.
    extract_script = """\
set -e
SYSROOT={sysroot}
# Start from a clean slate so a retried attempt doesn't trip over read-only
# (mode 0555) directories left behind by a partial extraction.
chmod -R u+w "$SYSROOT" 2>/dev/null || true
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT"
{docker} export {container_id} > "$SYSROOT/rootfs.tar"
for p in usr/include usr/lib usr/lib64 lib lib64; do
    tar -x -C "$SYSROOT" -f "$SYSROOT/rootfs.tar" "$p" 2>/dev/null || true
done
rm -f "$SYSROOT/rootfs.tar"

# Some container images ship lib dirs as mode 0555 (read-only). Ensure the
# user can write into directories so the symlink rewrite below can replace
# entries. The final chmod -R a+rX after this script restores read+exec for
# everyone.
find "$SYSROOT" -type d -exec chmod u+w {{}} +

# Rewrite absolute symlinks inside the sysroot to relative paths anchored at
# the sysroot root. Two reasons:
#   1. Linker correctness: scripts like libc.so reference absolute paths
#      (e.g. /lib64/ld-linux-x86-64.so.2). With the symlink kept absolute,
#      ld.lld follows it out of the sysroot to the host. Rewritten as a
#      relative link, it resolves to the loader inside the sysroot.
#   2. Bazel glob safety: an absolute symlink like usr/lib/ssl -> /etc/ssl
#      would have Bazel's glob descend into host paths and hit EACCES on
#      restricted dirs (/etc/ssl/private).
# If the target doesn't exist within the sysroot at all, drop the link.
find "$SYSROOT" -type l -lname '/*' | while IFS= read -r link; do
    target=$(readlink "$link")
    abs_in_sysroot="$SYSROOT$target"
    if [ -e "$abs_in_sysroot" ]; then
        rel=$(realpath --relative-to="$(dirname "$link")" "$abs_in_sysroot" 2>/dev/null || true)
        if [ -n "$rel" ]; then
            ln -sfn "$rel" "$link"
        fi
    else
        rm -f "$link"
    fi
done
""".format(docker = docker, sysroot = sysroot_dir, container_id = container_id)

    # `docker export` streams from the local daemon, but retry the script as a
    # whole in case the daemon hiccups mid-export; the script wipes and
    # recreates the sysroot dir on each attempt so retries start clean.
    result = retry_execute(
        ctx,
        tries = 3,
        arguments = ["bash", "-c", extract_script],
        environment = runtime_env,
        timeout = 600,
    )
    export_rc = result.return_code
    export_stderr = result.stderr

    # Always clean up the temporary container.
    ctx.execute([docker, "rm", "-f", container_id], environment = runtime_env, timeout = 30)

    if export_rc != 0:
        fail("Failed to export/extract container filesystem:\n{}".format(export_stderr))

    # Ensure all extracted files are readable by Bazel.
    ctx.execute(["chmod", "-R", "a+rX", str(sysroot_dir)], timeout = 60)

    # Verify the sysroot looks reasonable.
    if not ctx.path("sysroot/usr/include").exists:
        fail(
            "Extracted sysroot does not contain usr/include. " +
            "The container image may be empty or malformed.",
        )

    ctx.report_progress("RBE sysroot extracted successfully for {}".format(target_description))

    sysroot_path = "external/{}/sysroot".format(ctx.name)

    ctx.file("BUILD.bazel", _SYSROOT_BUILD)
    ctx.file(
        "sysroot_info.bzl",
        'SYSROOT_PATH = "{}"\nSYSROOT_ENABLED = True\nSYSROOT_CONTAINER_URL = "{}"\n'.format(
            sysroot_path,
            container_url,
        ),
    )

_NOOP_BUILD = """\
# RBE sysroot dump is not enabled.
# Set USE_RBE_SYSROOT=1 on an Amazon Linux 2023 host to extract the RBE
# container filesystem as a sysroot.

filegroup(
    name = "sysroot_files",
    srcs = [],
    visibility = ["//visibility:public"],
)
"""

_SYSROOT_BUILD = """\
# Extracted RBE container filesystem for use as a sysroot.

filegroup(
    name = "sysroot_files",
    # Exclude **/private to defensively skip restricted-mode dirs (e.g.
    # ssl/private) that the compiler never reads anyway.
    srcs = glob(
        ["sysroot/**/*"],
        exclude = ["sysroot/**/private/**", "sysroot/**/private"],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)
"""

sysroot_dump = repository_rule(
    implementation = _sysroot_dump_impl,
    environ = [
        SYSROOT_ENV_VAR,
        LINUX_CROSS_TOOLCHAIN_ENV_VAR,
    ] + _CONTAINER_RUNTIME_ENV_VARS,
    configure = True,
)
