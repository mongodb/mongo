#!/usr/bin/env python3
"""Script for downloading codeowners-validator."""

import hashlib
import os
import platform
import stat
import tarfile
import urllib.request
import zipfile
from typing import Annotated

import typer
from retry import retry

VALIDATOR_VERSION = "0.7.4"
VALIDATOR_BINARY_NAME = "codeowners-validator"
RELEASE_URL = (
    f"https://github.com/mszostok/codeowners-validator/releases/download/v{VALIDATOR_VERSION}/"
)

VALIDATOR_SHA256 = {
    "windows": {
        "x86_64": "4e71fcc686ad4f275a2fe9af0e3290a443e2907b07bd946f174109d5c057cda5",
        "i386": "08bc65d8773b264a1e45d7589d53aab05ce697f1ddb46ba15e0c7468b0574fb6",
    },
    "linux": {
        "x86_64": "73677228e3c7ddf3f9296246f2c088375c5b1b82385069360867d026ab790028",
        "i386": "6384762879d26c886da2a3ae9e99b076d1cf35749173ae99fea1ebb6c245b094",
        "arm64": "9286238af043e3bd42b2309530844e14ed51ec6c5c1aac9ac034068eb8d78668",
    },
    "darwin": {
        "x86_64": "25ae64da52eb2aad357af1275adb46fdf0c2d560def1cb9479800c775e65aa8e",
        "arm64": "efa77bf32d971181e007825a9c1b1239a690d8bab56e3e606e010c61511fd19e",
    },
}


def determine_platform():
    """Determine the operating system."""
    syst = platform.system()
    pltf = None
    if syst == "Darwin":
        pltf = "darwin"
    elif syst == "Windows":
        pltf = "windows"
    elif syst == "Linux":
        pltf = "linux"
    else:
        raise RuntimeError("Platform cannot be inferred.")
    return pltf


def determine_architecture():
    """Determine the CPU architecture."""
    arch = None
    machine = platform.machine().lower()
    if machine in ("amd64", "x86_64"):
        arch = "x86_64"
    elif machine in ("arm64", "aarch64"):
        arch = "arm64"
    elif machine in ("i386", "i686", "x86"):
        arch = "i386"
    else:
        raise RuntimeError(f"Detected architecture is not supported: {machine}")
    return arch


def sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


@retry(tries=3, delay=5)
def _download_with_retry(*args, **kwargs):
    return urllib.request.urlretrieve(*args, **kwargs)


def download_validator_binary(download_location: str):
    """Download the codeowners-validator binary."""

    # expand user to get absolute path
    download_location = os.path.expanduser(download_location)
    workspace_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")
    if workspace_dir:
        download_location = os.path.join(workspace_dir, download_location)

    operating_system = determine_platform()
    architecture = determine_architecture()

    if operating_system == "windows":
        extension = ".zip"
    else:
        extension = ".tar.gz"

    binary_name = (
        f"{VALIDATOR_BINARY_NAME}_{VALIDATOR_VERSION}_{operating_system}_{architecture}{extension}"
    )
    url = f"{RELEASE_URL}{binary_name}"

    # Download the archive
    archive_location = os.path.join(download_location, binary_name)
    _download_with_retry(url, archive_location)
    print(f"Downloaded codeowners-validator from {url} to {archive_location}")

    # Extract archive
    if operating_system == "windows":
        with zipfile.ZipFile(archive_location) as zip_ref:
            for file_name in zip_ref.namelist():
                if file_name == VALIDATOR_BINARY_NAME:
                    zip_ref.extract(file_name, download_location)
    else:
        with tarfile.open(archive_location) as tar:
            for member in tar.getmembers():
                if member.name == VALIDATOR_BINARY_NAME:
                    tar.extract(member, download_location)

    binary_path = os.path.join(
        download_location, VALIDATOR_BINARY_NAME + (".exe" if operating_system == "windows" else "")
    )

    expected_sha = VALIDATOR_SHA256.get(operating_system, {}).get(architecture)
    print(f"Expected SHA256: {expected_sha}")
    if not expected_sha:
        raise RuntimeError(f"No SHA256 hash found for {operating_system}/{architecture}")

    calculated_sha = sha256_file(binary_path)
    print(f"Calculated SHA256: {calculated_sha}")
    if calculated_sha != expected_sha:
        raise RuntimeError(
            f"Downloaded file from {url} calculated sha ({calculated_sha}) did not match expected sha ({expected_sha})"
        )

    # Set executable permissions on Unix-like systems
    if operating_system != "windows":
        os.chmod(binary_path, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        print(f"Set user executable permissions on {binary_path}")

    # Clean up archive
    os.remove(archive_location)


def main(
    download_location: Annotated[
        str,
        typer.Option(
            help="Directory to download the codeowners-validator binary to.",
        ),
    ] = "./",
):
    """Downloads codeowners-validator for use in evergreen and local development."""
    download_validator_binary(download_location=download_location)


if __name__ == "__main__":
    typer.run(main)
