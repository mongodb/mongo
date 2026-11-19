# Use to update remote_execution_containers.bzl with a new set of remote execution containers generated from
# the dockerfiles listed in bazel/remote_execution_container/.

import argparse
import gzip
import hashlib
import json
import os
import pathlib
import platform
import posixpath
import re
import subprocess
import tarfile
import tempfile
from datetime import datetime
from typing import Any

from retry import retry

from bazel.platforms.toolchain_sysroot_manifest import (
    TOOLCHAIN_SYSROOT_ARCHITECTURES as PUBLISHED_ARCHITECTURES,
)
from bazel.platforms.toolchain_sysroot_manifest import TOOLCHAIN_SYSROOT_DISTROS

DEFAULT_PLATFORMS = ("linux/amd64", "linux/arm64/v8")
RHEL_EXTRA_PLATFORMS = ("linux/ppc64le", "linux/s390x")
DISTRO_EXTRA_PLATFORMS = {distro: RHEL_EXTRA_PLATFORMS for distro in ("rhel8", "rhel9", "rhel10")}
ECR_PUBLIC_ALIAS = "w3i8j1a8"
ECR_REPOSITORY_NAME = "devprod-build"
ECR_REGISTRY = "public.ecr.aws"
ECR_REPOSITORY = f"{ECR_REGISTRY}/{ECR_PUBLIC_ALIAS}/{ECR_REPOSITORY_NAME}"
BINFMT_IMAGE = (
    "tonistiigi/binfmt@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0"
)
MACHINE_ARCHITECTURES = {
    "aarch64": "arm64",
    "x86_64": "amd64",
}
SYSROOT_PATHS = ("usr/include", "usr/lib", "usr/lib64", "lib", "lib64")
SYSROOT_EXCLUDED_PATHS = ("usr/lib/.build-id", "usr/lib64/.build-id")
TARGET_ELF = {
    "amd64": (62, 1, "ld-linux-x86-64.so.2"),
    "arm64": (183, 1, "ld-linux-aarch64.so.1"),
    "ppc64le": (21, 1, "ld64.so.2"),
    "s390x": (22, 2, "ld64.so.1"),
}


def _linker_script_inputs(contents: str) -> list[str]:
    """Read file operands in libc's GROUP/INPUT/AS_NEEDED directives."""
    contents = re.sub(r"/\*.*?\*/", "", contents, flags=re.DOTALL)
    directives = {"GROUP", "INPUT", "AS_NEEDED"}
    contexts: list[bool] = []
    previous = ""
    inputs = []
    for token in re.findall(r'"[^"]*"|[(){};]|[^\s(){};]+', contents):
        if token == "(":
            contexts.append(previous in directives or bool(contexts and contexts[-1]))
        elif token == ")":
            if not contexts:
                raise ValueError("Unbalanced parentheses in libc linker script")
            contexts.pop()
        elif contexts and contexts[-1] and token not in directives | {"{", "}", ";"}:
            inputs.append(token.strip('"').removeprefix("="))
        previous = token
    if contexts:
        raise ValueError("Unbalanced parentheses in libc linker script")
    return inputs


def validate_toolchain_sysroot(root: pathlib.Path, architecture: str) -> list[str]:
    """Check the target files used to configure/build GCC's runtime libraries."""
    machine, encoding, loader = TARGET_ELF[architecture]

    def validate_elf(path: pathlib.Path, *, allow_archive: bool = False) -> None:
        with path.open("rb") as source:
            header = source.read(20)
        if allow_archive and header.startswith(b"!<arch>\n"):
            return
        byteorder = "little" if encoding == 1 else "big"
        if (
            len(header) < 20
            or header[:5] != b"\x7fELF\x02"
            or header[5] != encoding
            or int.from_bytes(header[18:20], byteorder) != machine
        ):
            raise ValueError(f"Wrong ELF architecture for {architecture}: {path}")

    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            try:
                target = path.resolve(strict=True)
            except (OSError, RuntimeError) as exc:
                raise ValueError(f"Sysroot has a dangling or cyclic link: {path}") from exc
            if not target.is_relative_to(root.resolve()):
                raise ValueError(f"Sysroot link escapes the archive: {path}")
    validated = []
    for header in ("stdio.h", "linux/version.h", "asm/unistd.h", "sys/sdt.h", "crypt.h"):
        path = root / "usr/include" / header
        if not path.is_file():
            raise ValueError(f"Toolchain sysroot is missing usr/include/{header}")
        validated.append(path.relative_to(root).as_posix())
    loader_paths = [
        root / directory / loader for directory in ("usr/lib64", "lib64", "usr/lib", "lib")
    ]
    for script in (root / "usr/lib64/libc.so", root / "lib64/libc.so"):
        if not script.is_file():
            continue
        for token in _linker_script_inputs(script.read_text(encoding="utf-8")):
            reference = pathlib.PurePosixPath(token)
            if reference.is_absolute():
                path = root / reference.relative_to("/")
            elif len(reference.parts) > 1:
                path = script.parent / reference
            else:
                candidates = [script.parent / reference] + [
                    root / directory / reference
                    for directory in ("usr/lib64", "lib64", "usr/lib", "lib")
                ]
                path = next(
                    (candidate for candidate in candidates if candidate.is_file()), candidates[0]
                )
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError(f"{script.relative_to(root)} input reference escapes the sysroot")
            if not path.is_file():
                raise ValueError(f"{script.relative_to(root)} references missing input {reference}")
            # Libc scripts use ELF inputs and libc_nonshared.a. Check every ELF
            # operand, including loaders with unexpected or versioned filenames.
            validate_elf(path, allow_archive=True)
            validated.append(path.relative_to(root).as_posix())
            if reference.name == loader and path not in loader_paths:
                loader_paths.append(path)
    elf_files = ("crt1.o", "crti.o", "crtn.o", "libc.so.6", "libgcc_s.so.1", "libcrypt.so", loader)
    for name in (*elf_files, "libc.so", "libc_nonshared.a"):
        required = root / "usr/lib64" / name
        paths = [required, root / "lib64" / name]
        if name == loader:
            # RHEL 9/10 ARM64 installs its 64-bit loader in /usr/lib, exposed
            # through /lib. Validate every existing loader at these known paths.
            paths = loader_paths
            if not any(path.is_file() for path in paths):
                raise ValueError(f"Toolchain sysroot is missing target dynamic loader {name}")
        elif not required.is_file():
            # Copies elsewhere cannot replace GCC's required 64-bit linker inputs.
            raise ValueError(f"Toolchain sysroot is missing usr/lib64/{name}")
        for path in paths:
            if not path.is_file():
                continue
            if name in elf_files:
                validate_elf(path)
            validated.append(path.relative_to(root).as_posix())
    return sorted(set(validated))


def sysroot_provenance(
    image: str, distro: str, build_platform: str, image_identity: str
) -> dict[str, Any]:
    """Read target package versions from the exact image being exported."""
    query = (
        ["dpkg-query", "-W", "-f=${Package}=${Version}:${Architecture}\n"]
        if distro.startswith(("ubuntu", "debian"))
        else ["rpm", "-qa", "--qf", "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n"]
    )
    result = log_subprocess_run(
        ["docker", "run", "--rm", "--platform", build_platform, image, *query],
        capture_output=True,
        text=True,
        check=True,
    )
    packages = sorted(set(result.stdout.splitlines()))
    if not packages:
        raise ValueError(f"No target package inventory returned for {image}")
    return {
        "schema-version": 1,
        "distro": distro,
        "architecture": build_platform.split("/")[1],
        "image": image_identity,
        "packages": packages,
    }


def _is_sysroot_path(path: str) -> bool:
    # Debugger lookup indexes can reference executables omitted from this archive.
    return any(path == root or path.startswith(root + "/") for root in SYSROOT_PATHS) and not any(
        path == root or path.startswith(root + "/") for root in SYSROOT_EXCLUDED_PATHS
    )


def create_sysroot_archive(
    rootfs: pathlib.Path, archive: pathlib.Path, *, provenance: dict[str, Any] | None = None
) -> str:
    """Package headers/libraries with portable links and return the archive's SHA-256."""

    def link_target(name: str, link: str) -> str:
        return posixpath.normpath(
            link if link.startswith("/") else posixpath.join(posixpath.dirname(name), link)
        ).lstrip("/")

    def extract_filter(member: tarfile.TarInfo, destination: str) -> tarfile.TarInfo | None:
        name = posixpath.normpath(member.name.removeprefix("./"))
        if not _is_sysroot_path(name):
            return None
        member = member.replace(name=name)
        if member.issym():
            target = link_target(name, member.linkname)
            visited = set()
            while not _is_sysroot_path(target):
                # Preserve library selections routed through /etc/alternatives without
                # including system configuration in the compiler's sysroot.
                selected = exported_members.get(target)
                if selected is None or not selected.issym():
                    return None
                if target in visited:
                    raise ValueError(f"Sysroot link has a cyclic external target: {name}")
                visited.add(target)
                target = link_target(target, selected.linkname)
            member = member.replace(linkname=posixpath.relpath(target, posixpath.dirname(name)))
        elif member.islnk() and not _is_sysroot_path(member.linkname):
            raise ValueError(f"Sysroot hard link points outside the archive: {name}")
        return tarfile.data_filter(member, destination)

    def archive_filter(member: tarfile.TarInfo) -> tarfile.TarInfo:
        # data_filter leaves directory modes to the publisher's umask, including implicit parents.
        return member.replace(
            uid=0,
            gid=0,
            uname="",
            gname="",
            mtime=0,
            mode=0o755 if member.isdir() else member.mode,
            deep=False,
        )

    with tempfile.TemporaryDirectory(dir=rootfs.parent) as temp_dir:
        sysroot = pathlib.Path(temp_dir)
        with tarfile.open(rootfs) as exported:
            exported_members = {
                posixpath.normpath(member.name.removeprefix("./")): member
                for member in exported.getmembers()
            }
            exported.extractall(sysroot, filter=extract_filter)
        if not (sysroot / "usr/include").is_dir():
            raise ValueError("Extracted sysroot does not contain usr/include")
        if provenance is not None:
            manifest = dict(provenance)
            manifest["validated-files"] = (
                validate_toolchain_sysroot(sysroot, provenance["architecture"])
                if provenance["distro"] in TOOLCHAIN_SYSROOT_DISTROS
                else []
            )
            manifest_path = sysroot / "sysroot-manifest.json"
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            manifest_path.chmod(0o644)
        with (
            archive.open("wb") as output,
            gzip.GzipFile(filename="", fileobj=output, mode="wb", mtime=0) as compressed,
            tarfile.open(fileobj=compressed, mode="w") as packaged,
        ):
            for path in sorted(sysroot.rglob("*")):
                packaged.add(
                    path,
                    arcname=path.relative_to(sysroot).as_posix(),
                    recursive=False,
                    filter=archive_filter,
                )
    with archive.open("rb") as output:
        return hashlib.file_digest(output, "sha256").hexdigest()


def sysroot_upload_environment() -> dict[str, str]:
    """Use Evergreen's public-build S3 credentials independently of the ECR role."""
    environment = os.environ.copy()
    access_key = environment.get("aws_key_build")
    secret_key = environment.get("aws_secret_build")
    if access_key or secret_key:
        if not access_key or not secret_key:
            raise ValueError("Both aws_key_build and aws_secret_build are required for S3 uploads")
        environment["AWS_ACCESS_KEY_ID"] = access_key
        environment["AWS_SECRET_ACCESS_KEY"] = secret_key
        environment.pop("AWS_SESSION_TOKEN", None)
    return environment


def publish_sysroots(
    image: str, distro: str, bucket: str, prefix: str, *, dry_run: bool = False
) -> dict[str, str]:
    """Export each platform and prepare its S3 metadata; skip uploads during patch dry runs."""
    result = log_subprocess_run(
        ["docker", "buildx", "imagetools", "inspect", "--raw", image],
        capture_output=True,
        text=True,
        check=True,
    )
    manifests = json.loads(result.stdout).get("manifests", [])
    metadata = {}
    for build_platform in platforms_for_distro(distro):
        architecture = build_platform.split("/")[1]
        manifest = next(
            (
                manifest
                for manifest in manifests
                if manifest.get("platform", {}).get("os") == "linux"
                and manifest["platform"].get("architecture") == architecture
            ),
            None,
        )
        if manifest is None:
            raise ValueError(f"Image {image} has no linux/{architecture} manifest")
        # Use the child digest: Docker cannot cache multiple architectures under one index digest.
        platform_image = image.split("@")[0] + "@" + manifest["digest"]
        log_subprocess_run(
            ["docker", "pull", "--platform", build_platform, platform_image], check=True
        )
        container_id = ""
        try:
            result = log_subprocess_run(
                ["docker", "create", "--platform", build_platform, platform_image, "/bin/true"],
                capture_output=True,
                text=True,
                check=True,
            )
            container_id = result.stdout.strip()
            if not container_id:
                raise ValueError("docker create did not return a container ID for sysroot export")
            # Keep one architecture on disk at a time, beside the repo on the large CI data volume.
            with tempfile.TemporaryDirectory(prefix="rbe-sysroot-", dir=".") as temp_dir:
                directory = pathlib.Path(temp_dir)
                rootfs = directory / "rootfs.tar"
                log_subprocess_run(
                    ["docker", "export", "--output", str(rootfs), container_id], check=True
                )
                archive = directory / "sysroot.tar.gz"
                sha256 = create_sysroot_archive(rootfs, archive)
                filename = f"rbe-sysroot-{distro}-{architecture}-{sha256}.tar.gz"
                key = "/".join(part for part in (prefix.strip("/"), filename) if part)
                if dry_run:
                    print(f"Dry run: would upload {archive} to s3://{bucket}/{key}")
                else:
                    log_subprocess_run(
                        [
                            "aws",
                            "s3",
                            "cp",
                            str(archive),
                            f"s3://{bucket}/{key}",
                            "--acl",
                            "public-read",
                        ],
                        env=sysroot_upload_environment(),
                        check=True,
                    )
                metadata[f"sysroot-{architecture}-url"] = f"https://{bucket}.s3.amazonaws.com/{key}"
                metadata[f"sysroot-{architecture}-sha256"] = sha256
        finally:
            if container_id:
                log_subprocess_run(["docker", "rm", "--force", container_id], check=False)
            log_subprocess_run(["docker", "image", "rm", platform_image], check=False)
    return metadata


def platforms_for_distro(distro: str) -> tuple[str, ...]:
    """Return the architectures published for a distro's image manifest."""
    return DEFAULT_PLATFORMS + DISTRO_EXTRA_PLATFORMS.get(distro, ())


def resolve_dockerfile(configured_path: pathlib.Path) -> pathlib.Path:
    """Resolve generated lowercase Dockerfiles referenced by older mappings."""
    if configured_path.is_file():
        return configured_path
    generated_path = configured_path.with_name("dockerfile")
    if generated_path.is_file():
        return generated_path
    raise FileNotFoundError(f"Could not find Dockerfile at {configured_path} or {generated_path}")


def emulated_architectures(machine: str | None = None) -> tuple[str, ...]:
    """Return published architectures that need QEMU on the current host."""
    native_architecture = MACHINE_ARCHITECTURES.get(machine or platform.machine())
    return tuple(arch for arch in PUBLISHED_ARCHITECTURES if arch != native_architecture)


def _log_subprocess_run(*args, **kwargs):
    arg_list_or_string = kwargs["args"] if "args" in kwargs else args[0]
    print(" ".join(arg_list_or_string) if type(arg_list_or_string) == list else arg_list_or_string)
    return subprocess.run(*args, **kwargs)


@retry(tries=3)
def log_subprocess_run(*args, **kwargs):
    return _log_subprocess_run(*args, **kwargs)


@retry(tries=3)
def create_buildx_builder(builder_name: str):
    """Create a fresh Buildx builder, including after a failed bootstrap attempt."""
    _log_subprocess_run(
        ["docker", "buildx", "rm", "--force", builder_name],
        check=False,
    )
    return _log_subprocess_run(
        [
            "docker",
            "buildx",
            "create",
            "--name",
            builder_name,
            "--driver",
            "docker-container",
            "--use",
            "--bootstrap",
        ],
        check=True,
    )


def web_url_for_image(image: str) -> str:
    """Return the ECR Public gallery URL for a digest-pinned image reference."""
    digest = image.split("@", 1)[1]
    return f"https://gallery.ecr.aws/{ECR_PUBLIC_ALIAS}/{ECR_REPOSITORY_NAME}/{digest}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--distro", type=str, help="Restrict to only update a single distro.")
    parser.add_argument(
        "--sysroot-s3-bucket",
        help="Also export and publish a sysroot for each architecture to this public S3 bucket.",
    )
    parser.add_argument(
        "--sysroot-s3-prefix", default="rbe-sysroots", help="S3 key prefix for sysroot archives."
    )
    parser.add_argument(
        "--sysroot-dry-run",
        action="store_true",
        help="Extract and hash sysroots and print proposed S3 URLs without uploading archives.",
    )
    parser.add_argument(
        "--skip-cleanup",
        action="store_true",
        help="Disable cleanup between container builds. This requires a large amount of disk space.",
    )
    parser.add_argument(
        "--install-binfmt",
        action="store_true",
        help=(
            "Install QEMU binfmt handlers for all non-native publication platforms. "
            "This runs a privileged disposable container."
        ),
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help=(
            "Keep updating the remaining distros when one fails to build, and report the "
            "failures at the end."
        ),
    )
    args = parser.parse_args()

    if not args.skip_cleanup:
        user_input = input("""Warning: to build remote execution containers the docker data on the host system must be 
purged after each step to preserve disk space.

Pass --skip-cleanup to disable this step, but be aware that this will require a large amount of disk space.

Your docker images, volumes and containers will be purged if you continue. Enter 'y' to continue or 'n' to exit:""")
        if user_input.lower() != "y":
            return 0

    remote_execution_containers = {}
    container_file_path = os.path.join(
        pathlib.Path(__file__).parent.resolve(), "remote_execution_containers.bzl"
    )
    with open(container_file_path, "r") as f:
        code = compile(f.read(), container_file_path, "exec")
        exec(code, {}, remote_execution_containers)

    binfmt_ready = False
    builder_name = f"mongodb-rbe-publisher-{os.getpid()}"
    failed_distros = []
    for distro, re_container in remote_execution_containers["REMOTE_EXECUTION_CONTAINERS"].items():
        if args.distro is not None:
            if distro != args.distro:
                continue

        if not args.skip_cleanup:
            # Clean host system between container builds to avoid running into disk space issues.
            print("Cleaning host system's docker images, containers, volumes, and networks...")
            for command in [
                "docker stop $(docker ps -a -q)",  # Stop all running containers
                "docker rm $(docker ps -a -q)",  # Remove all containers
                "docker rmi $(docker images -q)",  # Remove all images
                "docker volume rm $(docker volume ls -q)",  # Remove all volumes
            ]:
                log_subprocess_run(command, shell=True)

        dockerfile = resolve_dockerfile(pathlib.Path(re_container["dockerfile"]))
        tag = f"{ECR_REPOSITORY}:{distro}-{datetime.now().strftime('%Y_%m_%d-%H_%M_%S')}"

        print(f"Updating {distro} container...")
        print(f"Using dockerfile: {dockerfile}")
        print(f"Using tag: {tag}\n")

        if args.install_binfmt and not binfmt_ready:
            log_subprocess_run(
                [
                    "docker",
                    "run",
                    "--privileged",
                    "--rm",
                    BINFMT_IMAGE,
                    "--install",
                    ",".join(emulated_architectures()),
                ],
                check=True,
            )
            binfmt_ready = True

        try:
            create_buildx_builder(builder_name)
            log_subprocess_run(
                [
                    "docker",
                    "buildx",
                    "build",
                    "--builder",
                    builder_name,
                    "--push",
                    "--platform",
                    ",".join(platforms_for_distro(distro)),
                    "--file",
                    str(dockerfile.resolve()),
                    "--tag",
                    tag,
                    str(dockerfile.parent.resolve()),
                ],
                check=True,
            )
            log_subprocess_run(["docker", "pull", tag], check=True)
            result = log_subprocess_run(
                ["docker", "inspect", "--format='{{.RepoDigests}}'", tag],
                capture_output=True,
                text=True,
                check=True,
            )
            # The output is a list of strings, ex. ['URL']; strip the brackets and quotes.
            image = result.stdout.strip()[2:-2]
            sysroot_metadata = (
                publish_sysroots(
                    image,
                    distro,
                    args.sysroot_s3_bucket,
                    args.sysroot_s3_prefix,
                    dry_run=args.sysroot_dry_run,
                )
                if args.sysroot_s3_bucket
                else {}
            )
        except (subprocess.CalledProcessError, OSError, ValueError, tarfile.TarError) as exc:
            if not args.continue_on_error:
                raise
            print(f"Failed to update {distro}: {exc}")
            print("************************************\n")
            failed_distros.append(distro)
            continue
        finally:
            log_subprocess_run(["docker", "buildx", "rm", "--force", builder_name], check=False)

        # Record the new pins together only once every requested sysroot upload has succeeded.
        for key in list(re_container):
            if key.startswith("sysroot-"):
                del re_container[key]
        re_container["container-url"] = "docker://" + image
        re_container["web-url"] = web_url_for_image(image)
        re_container.update(sysroot_metadata)

        print(f"Finished updating {distro}")
        print("************************************\n")

        with open(container_file_path, "w") as f:
            print(f"Writing remote execution container map to {container_file_path}...")
            print(
                "# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.\n",
                file=f,
            )

            # Manually print out the dict to maintain the trailing comma in each last element to satisfy the buildifier lint rules and
            # avoid reformating.
            print("REMOTE_EXECUTION_CONTAINERS = {", file=f)
            for key, value in sorted(
                remote_execution_containers["REMOTE_EXECUTION_CONTAINERS"].items(),
                key=lambda x: x[0],
            ):
                print(f'    "{key}": {{', file=f)
                for subkey, subvalue in sorted(value.items(), key=lambda x: x[0]):
                    print(f'        "{subkey}": "{subvalue}",', file=f)
                print("    },", file=f)
            print("}", file=f)

        with open(container_file_path, "r") as f:
            print(f"Finished writing to {container_file_path}:")
            print(f.read())

    if failed_distros:
        print(f"Failed to update these distros: {', '.join(failed_distros)}")
        return 1

    return 0


if __name__ == "__main__":
    exit(main())
