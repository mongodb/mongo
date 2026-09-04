# Use to update remote_execution_containers.bzl with a new set of remote execution containers generated from
# the dockerfiles listed in bazel/remote_execution_container/.

import argparse
import os
import pathlib
import platform
import subprocess
from datetime import datetime

from retry import retry

DEFAULT_PLATFORMS = ("linux/amd64", "linux/arm64/v8")
RHEL_EXTRA_PLATFORMS = ("linux/ppc64le", "linux/s390x")
DISTRO_EXTRA_PLATFORMS = {distro: RHEL_EXTRA_PLATFORMS for distro in ("rhel8", "rhel9", "rhel10")}
BINFMT_IMAGE = (
    "tonistiigi/binfmt@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0"
)
PUBLISHED_ARCHITECTURES = ("amd64", "arm64", "ppc64le", "s390x")
MACHINE_ARCHITECTURES = {
    "aarch64": "arm64",
    "x86_64": "amd64",
}


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--distro", type=str, help="Restrict to only update a single distro.")
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
        tag = f"quay.io/mongodb/bazel-remote-execution:{distro}-{datetime.now().strftime('%Y_%m_%d-%H_%M_%S')}"

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
        finally:
            log_subprocess_run(["docker", "buildx", "rm", "--force", builder_name], check=False)

        log_subprocess_run(["docker", "pull", tag], check=True)
        result = log_subprocess_run(
            ["docker", "inspect", "--format='{{.RepoDigests}}'", tag],
            capture_output=True,
            text=True,
            check=True,
        )

        # The output of this command is a list of strings, ex. ['URL'] so we need to strip off the brackets and quotes.
        re_container["container-url"] = "docker://" + result.stdout.strip()[2:-2]
        re_container["web-url"] = "https://" + result.stdout.strip()[2:-2].replace(
            "quay.io/", "quay.io/repository/"
        ).replace("@sha256", "/manifest/sha256")

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

    return 0


if __name__ == "__main__":
    exit(main())
