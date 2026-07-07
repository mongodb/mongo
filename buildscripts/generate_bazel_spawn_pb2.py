#!/usr/bin/env python3
"""Regenerate the Python protobuf module for Bazel compact execution logs."""

import argparse
import difflib
import hashlib
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

MONGODB_BAZEL_FORK = "https://github.com/mongodb-forks/bazel"
MONGODB_BAZEL_VERSION = "7.5.0-mongo_90b8a7b661"
MONGODB_BAZEL_REF = "90b8a7b661043fc7241aecddd17d39e75c8efd23"
SPAWN_PROTO_RELATIVE_PATH = "src/main/protobuf/spawn.proto"
SPAWN_PROTO_RAW_URL = (
    "https://raw.githubusercontent.com/mongodb-forks/bazel/"
    f"{MONGODB_BAZEL_REF}/{SPAWN_PROTO_RELATIVE_PATH}"
)
DEFAULT_OUTPUT = Path(__file__).with_name("bazel_spawn_pb2.py")
# Match the Python protobuf runtime pinned in poetry.lock.
PROTOC_VERSION = "29.3"
PROTOC_RELEASE_BASE_URL = (
    "https://github.com/protocolbuffers/protobuf/releases/download/" f"v{PROTOC_VERSION}"
)
PROTOC_ARCHIVES = {
    "linux-aarch_64": (
        f"protoc-{PROTOC_VERSION}-linux-aarch_64.zip",
        "6427349140e01f06e049e707a58709a4f221ae73ab9a0425bc4a00c8d0e1ab32",
    ),
    "linux-ppcle_64": (
        f"protoc-{PROTOC_VERSION}-linux-ppcle_64.zip",
        "0e9894ec2e3992b14d183e7ceac16465d6a6ee73e1d234695d80e6d1e947014c",
    ),
    "linux-s390_64": (
        f"protoc-{PROTOC_VERSION}-linux-s390_64.zip",
        "637857fdbab0b1334bdb2b08733f0be49685e693011b6104809491ac62fbd4d5",
    ),
    "linux-x86_64": (
        f"protoc-{PROTOC_VERSION}-linux-x86_64.zip",
        "3e866620c5be27664f3d2fa2d656b5f3e09b5152b42f1bedbf427b333e90021a",
    ),
    "osx-aarch_64": (
        f"protoc-{PROTOC_VERSION}-osx-aarch_64.zip",
        "2b8a3403cd097f95f3ba656e14b76c732b6b26d7f183330b11e36ef2bc028765",
    ),
    "osx-x86_64": (
        f"protoc-{PROTOC_VERSION}-osx-x86_64.zip",
        "9a788036d8f9854f7b03c305df4777cf0e54e5b081e25bf15252da87e0e90875",
    ),
    "win32": (
        f"protoc-{PROTOC_VERSION}-win32.zip",
        "c7c8028c1c4d801c53602920f2c86892054086bd965b6b23a4ba95d211dcb1d4",
    ),
    "win64": (
        f"protoc-{PROTOC_VERSION}-win64.zip",
        "57ea59e9f551ad8d71ffaa9b5cfbe0ca1f4e720972a1db7ec2d12ab44bff9383",
    ),
}


def download_spawn_proto(output_path: Path) -> None:
    """Download Bazel's spawn.proto from the pinned MongoDB Bazel fork ref."""

    request = urllib.request.Request(
        SPAWN_PROTO_RAW_URL,
        headers={"User-Agent": "mongodb-bazel-spawn-pb2-generator"},
    )
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            output_path.write_bytes(response.read())
    except urllib.error.URLError as exc:
        raise RuntimeError(
            f"Failed to download {SPAWN_PROTO_RELATIVE_PATH} from "
            f"{MONGODB_BAZEL_FORK} at {MONGODB_BAZEL_VERSION} "
            f"({MONGODB_BAZEL_REF}): {exc}"
        ) from exc


def download_file_with_sha256(url: str, output_path: Path, expected_sha256: str) -> None:
    """Download a file and verify its SHA256 digest."""

    request = urllib.request.Request(
        url,
        headers={"User-Agent": "mongodb-bazel-spawn-pb2-generator"},
    )
    digest = hashlib.sha256()
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            with output_path.open("wb") as file_handle:
                while chunk := response.read(1024 * 1024):
                    digest.update(chunk)
                    file_handle.write(chunk)
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Failed to download {url}: {exc}") from exc

    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
        output_path.unlink(missing_ok=True)
        raise RuntimeError(
            f"SHA256 mismatch for {url}: expected {expected_sha256}, found {actual_sha256}"
        )


def protoc_platform_key() -> str:
    """Return the protoc release platform key for the current host."""

    machine = platform.machine().lower()
    if sys.platform.startswith("linux"):
        if machine in {"aarch64", "arm64"}:
            return "linux-aarch_64"
        if machine in {"ppc64le", "powerpc64le"}:
            return "linux-ppcle_64"
        if machine == "s390x":
            return "linux-s390_64"
        if machine in {"amd64", "x86_64"}:
            return "linux-x86_64"

    if sys.platform == "darwin":
        if machine in {"aarch64", "arm64"}:
            return "osx-aarch_64"
        if machine in {"amd64", "x86_64"}:
            return "osx-x86_64"

    if sys.platform in {"win32", "cygwin"}:
        if machine in {"amd64", "x86_64"}:
            return "win64"
        if machine in {"i386", "i686", "x86"}:
            return "win32"

    raise RuntimeError(f"No pinned protoc {PROTOC_VERSION} archive for {sys.platform}/{machine}")


def resolve_explicit_protoc(protoc: str) -> Path:
    """Resolve a user-supplied protoc executable."""

    protoc_path = Path(protoc)
    if protoc_path.is_file():
        return protoc_path

    protoc_command = shutil.which(protoc)
    if protoc_command:
        return Path(protoc_command)

    raise RuntimeError(f"Could not find protoc executable {protoc!r}")


def download_pinned_protoc(temp_path: Path) -> tuple[Path, Path]:
    """Download and extract the pinned protoc release for the current host."""

    platform_key = protoc_platform_key()
    archive_name, expected_sha256 = PROTOC_ARCHIVES[platform_key]
    archive_url = f"{PROTOC_RELEASE_BASE_URL}/{archive_name}"
    archive_path = temp_path / archive_name
    extract_path = temp_path / "protoc"

    download_file_with_sha256(archive_url, archive_path, expected_sha256)
    with zipfile.ZipFile(archive_path) as archive:
        archive.extractall(extract_path)

    protoc_binary = (
        extract_path / "bin" / ("protoc.exe" if platform_key.startswith("win") else "protoc")
    )
    if not protoc_binary.is_file():
        raise RuntimeError(f"Downloaded protoc archive did not contain {protoc_binary}")

    protoc_binary.chmod(protoc_binary.stat().st_mode | stat.S_IXUSR)
    include_path = extract_path / "include"
    if not include_path.is_dir():
        raise RuntimeError(f"Downloaded protoc archive did not contain {include_path}")

    return protoc_binary, include_path


def protobuf_include_paths() -> list[Path]:
    """Return optional proto include roots for well-known Google protobuf types."""

    try:
        import google.protobuf
    except ImportError:
        return []

    protobuf_package = Path(google.protobuf.__file__).resolve().parent
    site_packages = protobuf_package.parent.parent
    if (site_packages / "google/protobuf/duration.proto").is_file():
        return [site_packages]
    return []


def generate_pb2(
    protoc: Path, proto_path: Path, output_dir: Path, include_paths: list[Path]
) -> Path:
    """Generate spawn_pb2.py from spawn.proto."""

    proto_root = proto_path.parent
    command = [
        str(protoc),
        f"--proto_path={proto_root}",
        *(f"--proto_path={include_path}" for include_path in include_paths),
        *(f"--proto_path={include_path}" for include_path in protobuf_include_paths()),
        f"--python_out={output_dir}",
        str(proto_path),
    ]
    subprocess.run(command, check=True)

    generated_path = output_dir / "spawn_pb2.py"
    if not generated_path.is_file():
        raise RuntimeError(f"protoc did not create expected output: {generated_path}")
    return generated_path


def update_output(generated_path: Path, output_path: Path, check: bool) -> None:
    """Write or verify the generated pb2 output."""

    generated_text = generated_path.read_text(encoding="utf-8")
    if check:
        if not output_path.is_file():
            raise RuntimeError(f"Generated output is missing: {output_path}")

        existing_text = output_path.read_text(encoding="utf-8")
        if existing_text != generated_text:
            diff = "".join(
                difflib.unified_diff(
                    existing_text.splitlines(keepends=True),
                    generated_text.splitlines(keepends=True),
                    fromfile=str(output_path),
                    tofile="regenerated",
                )
            )
            raise RuntimeError(f"{output_path} is out of date:\n{diff}")
        print(f"{output_path} is up to date")
        return

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(generated_text, encoding="utf-8")
    print(
        f"Generated {output_path} from {MONGODB_BAZEL_FORK} "
        f"{MONGODB_BAZEL_VERSION} ({MONGODB_BAZEL_REF}):{SPAWN_PROTO_RELATIVE_PATH}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate the Python protobuf module for Bazel compact execution logs."
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"Output pb2 module path. Defaults to {DEFAULT_OUTPUT}.",
    )
    parser.add_argument(
        "--protoc",
        help=(
            "Use this protoc executable instead of downloading the pinned "
            f"protoc {PROTOC_VERSION} release."
        ),
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify the output file is already up to date instead of writing it.",
    )
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="bazel-spawn-pb2-") as temp_dir:
        temp_path = Path(temp_dir)
        proto_path = temp_path / "spawn.proto"
        if args.protoc:
            protoc, include_paths = resolve_explicit_protoc(args.protoc), []
        else:
            protoc, include_path = download_pinned_protoc(temp_path)
            include_paths = [include_path]
        download_spawn_proto(proto_path)
        generated_path = generate_pb2(protoc, proto_path, temp_path, include_paths)
        update_output(generated_path, args.output, args.check)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
