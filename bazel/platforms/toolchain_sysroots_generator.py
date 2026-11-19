"""Build target runtime sysroots without publishing container images."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tarfile
import tempfile
from typing import Any

from bazel.platforms import remote_execution_containers_generator as containers
from bazel.platforms.toolchain_sysroot_manifest import read_sysroot_manifest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "bazel/toolchains/target_sysroots.json"


def build_sysroots(
    distro: str, output: pathlib.Path, architectures: set[str] | None
) -> dict[str, dict[str, str]]:
    """Repin a runtime-only recipe, build local images, and export validated archives."""
    recipes = output / "dockerfiles" / distro
    containers.log_subprocess_run(
        [
            "bash",
            str(REPO_ROOT / "bazel/remote_execution_container/repin_dockerfiles.sh"),
            "--toolchain-sysroots",
            "--output-dir",
            str(recipes),
        ],
        env={**os.environ, "toolchain_sysroot_distro": distro},
        check=True,
    )
    dockerfiles = list(recipes.glob("*/dockerfile"))
    if len(dockerfiles) != 1:
        raise ValueError(
            f"Expected one generated toolchain recipe for {distro}, found {len(dockerfiles)}"
        )
    dockerfile = dockerfiles[0]
    recipe_bytes = dockerfile.read_bytes()
    recipe = recipe_bytes.decode("utf-8")
    base_images = re.findall(r"^FROM (\S+)$", recipe, flags=re.MULTILINE)
    if len(base_images) != 1 or not re.fullmatch(r".+@sha256:[0-9a-f]{64}", base_images[0]):
        raise ValueError(f"Expected one digest-pinned base image in {dockerfile}")
    base_image = base_images[0]
    recipe_sha256 = hashlib.sha256(recipe_bytes).hexdigest()
    builder = f"mongodb-toolchain-sysroots-{os.getpid()}"
    metadata = {}
    try:
        containers.create_buildx_builder(builder)
        for build_platform in containers.platforms_for_distro(distro):
            architecture = build_platform.split("/")[1]
            if architectures is not None and architecture not in architectures:
                continue
            tag = f"mongodb-toolchain-sysroot:{distro}-{architecture}-{os.getpid()}"
            container_id = ""
            try:
                containers.log_subprocess_run(
                    [
                        "docker",
                        "buildx",
                        "build",
                        "--builder",
                        builder,
                        "--load",
                        "--platform",
                        build_platform,
                        "--file",
                        str(dockerfile),
                        "--tag",
                        tag,
                        str(dockerfile.parent),
                    ],
                    check=True,
                )
                image_id = containers.log_subprocess_run(
                    ["docker", "inspect", "--format={{.Id}}", tag],
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
                if not image_id:
                    raise ValueError(f"No image ID returned for {tag}")
                provenance = containers.sysroot_provenance(tag, distro, build_platform, image_id)
                provenance.update(
                    {
                        "purpose": "cross-toolchain-runtime",
                        "recipe-sha256": recipe_sha256,
                        "recipe": recipe,
                        "base-image": base_image,
                    }
                )
                container_id = containers.log_subprocess_run(
                    ["docker", "create", "--platform", build_platform, tag, "/bin/true"],
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
                if not container_id:
                    raise ValueError(f"No container ID returned for {tag}")
                with tempfile.TemporaryDirectory(prefix="sysroot-", dir=output) as tmp:
                    rootfs, archive = (
                        pathlib.Path(tmp) / "rootfs.tar",
                        pathlib.Path(tmp) / "sysroot.tar.gz",
                    )
                    containers.log_subprocess_run(
                        ["docker", "export", "--output", str(rootfs), container_id], check=True
                    )
                    sha256 = containers.create_sysroot_archive(
                        rootfs, archive, provenance=provenance
                    )
                    filename = f"toolchain-sysroot-{distro}-{architecture}-{sha256}.tar.gz"
                    archive.replace(output / filename)
                metadata[architecture] = {
                    "archive": filename,
                    "sha256": sha256,
                    "image-id": image_id,
                    "base-image": base_image,
                    "recipe-sha256": recipe_sha256,
                }
                print(f"Validated {distro}/{architecture}: {filename}")
            finally:
                if container_id:
                    containers.log_subprocess_run(
                        ["docker", "rm", "--force", container_id], check=False
                    )
                containers.log_subprocess_run(["docker", "image", "rm", tag], check=False)
    finally:
        containers.log_subprocess_run(["docker", "buildx", "rm", "--force", builder], check=False)
    return metadata


def publish_sysroots(
    distro: str, output: pathlib.Path, metadata: dict[str, dict[str, str]], bucket: str, prefix: str
) -> dict[str, dict[str, str]]:
    """Upload validated archives; only return pins after every target upload succeeds."""
    pins = {}
    for architecture, artifact in metadata.items():
        key = "/".join(part for part in (prefix.strip("/"), artifact["archive"]) if part)
        pins[architecture] = {
            "url": f"https://{bucket}.s3.amazonaws.com/{key}",
            **{name: value for name, value in artifact.items() if name != "archive"},
        }
    read_sysroot_manifest(
        json.dumps({"schema-version": 1, "sysroots": {distro: pins}}),
        source=f"Published {distro} pins",
        allow_empty=False,
    )
    for architecture, artifact in metadata.items():
        key = "/".join(part for part in (prefix.strip("/"), artifact["archive"]) if part)
        containers.log_subprocess_run(
            [
                "aws",
                "s3",
                "cp",
                str(output / artifact["archive"]),
                f"s3://{bucket}/{key}",
                "--acl",
                "public-read",
            ],
            env=containers.sysroot_upload_environment(),
            check=True,
        )
    return pins


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--publish", action="store_true")
    mode.add_argument("--no-push", action="store_true")
    parser.add_argument("--distro", choices=containers.TOOLCHAIN_SYSROOT_DISTROS)
    parser.add_argument(
        "--architectures", help="Optional comma-separated subset for no-push validation."
    )
    parser.add_argument(
        "--output-dir", type=pathlib.Path, default=pathlib.Path("toolchain-sysroot-output")
    )
    parser.add_argument("--manifest", type=pathlib.Path, default=MANIFEST)
    parser.add_argument("--s3-bucket", default="mdb-build-public")
    parser.add_argument("--s3-prefix", default="toolchain-sysroots")
    parser.add_argument("--install-binfmt", action="store_true")
    args = parser.parse_args(argv)
    if args.publish and os.environ.get("is_patch", "false").lower() == "true":
        parser.error("Patch builds must use --no-push")
    architectures = set(args.architectures.split(",")) if args.architectures else None
    if architectures is not None:
        if args.publish or args.distro is None:
            parser.error("--architectures requires --no-push and --distro")
        if not architectures <= set(containers.PUBLISHED_ARCHITECTURES):
            parser.error(f"Unsupported architectures: {args.architectures}")
    manifest = None
    if args.publish:
        try:
            manifest = read_sysroot_manifest(
                args.manifest.read_text(encoding="utf-8"),
                source=str(args.manifest),
                allow_empty=False,
            )
        except ValueError as exc:
            parser.error(str(exc))
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.install_binfmt:
        containers.log_subprocess_run(
            [
                "docker",
                "run",
                "--privileged",
                "--rm",
                containers.BINFMT_IMAGE,
                "--install",
                ",".join(containers.emulated_architectures()),
            ],
            check=True,
        )
    results: dict[str, Any] = {}
    pins = {}
    failed = []
    for distro in (args.distro,) if args.distro else containers.TOOLCHAIN_SYSROOT_DISTROS:
        try:
            results[distro] = build_sysroots(distro, output, architectures)
            if args.publish:
                pins[distro] = publish_sysroots(
                    distro, output, results[distro], args.s3_bucket, args.s3_prefix
                )
        except (subprocess.CalledProcessError, OSError, ValueError, tarfile.TarError) as exc:
            failed.append(distro)
            print(f"Failed {distro}: {exc}")
    (output / "validation.json").write_text(
        json.dumps({"sysroots": results, "failed_distros": failed}, indent=2, sort_keys=True) + "\n"
    )
    if failed:
        return 1
    if args.publish:
        assert manifest is not None
        manifest["sysroots"].update(pins)
        with tempfile.NamedTemporaryFile(
            mode="w", dir=args.manifest.parent, delete=False
        ) as updated:
            json.dump(manifest, updated, indent=2, sort_keys=True)
            updated.write("\n")
        updated_path = pathlib.Path(updated.name)
        updated_path.chmod(0o644)
        updated_path.replace(args.manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
