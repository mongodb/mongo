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
import subprocess
import tarfile
import tempfile
from datetime import datetime

from retry import retry

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
PUBLISHED_ARCHITECTURES = ("amd64", "arm64", "ppc64le", "s390x")
MACHINE_ARCHITECTURES = {
    "aarch64": "arm64",
    "x86_64": "amd64",
}
SYSROOT_PATHS = ("usr/include", "usr/lib", "usr/lib64", "lib", "lib64")


def _is_sysroot_path(path: str) -> bool:
    return any(path == root or path.startswith(root + "/") for root in SYSROOT_PATHS)


def create_sysroot_archive(rootfs: pathlib.Path, archive: pathlib.Path) -> str:
    """Package headers/libraries with portable links and return the archive's SHA-256."""

    def extract_filter(member: tarfile.TarInfo, destination: str) -> tarfile.TarInfo | None:
        name = posixpath.normpath(member.name.removeprefix("./"))
        if not _is_sysroot_path(name):
            return None
        member = member.replace(name=name)
        if member.issym():
            target = posixpath.normpath(
                member.linkname
                if member.linkname.startswith("/")
                else posixpath.join(posixpath.dirname(name), member.linkname)
            ).lstrip("/")
            if not _is_sysroot_path(target):
                return None
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
            exported.extractall(sysroot, filter=extract_filter)
        if not (sysroot / "usr/include").is_dir():
            raise ValueError("Extracted sysroot does not contain usr/include")
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
