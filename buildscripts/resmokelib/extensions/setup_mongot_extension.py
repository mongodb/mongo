#!/usr/bin/env python3
import logging
import os
import platform
import shutil
import tarfile
import tempfile
from typing import Optional

from buildscripts.resmokelib.extensions.constants import (
    CONF_OUT_DIR,
    EVERGREEN_SEARCH_DIRS,
    LOCAL_SEARCH_DIRS,
)
from buildscripts.s3_binary.download import download_s3_binary

SO_FILENAME = "mongot-extension.so"
CONF_FILENAME = "mongot-extension.conf"


def get_so_path(is_evergreen: bool) -> str:
    """Get the full path to the mongot-extension .so file."""
    search_dirs = EVERGREEN_SEARCH_DIRS if is_evergreen else LOCAL_SEARCH_DIRS
    install_dir = next((d for d in search_dirs if os.path.isdir(d)), search_dirs[0])
    return os.path.join(install_dir, SO_FILENAME)


def get_tarball_url() -> str:
    """Build the S3 URL for the mongot-extension tarball based on platform and architecture."""
    # Detect architecture.
    arch = platform.machine()
    if arch == "x86_64":
        arch_str = "x86_64"
    elif arch in ("aarch64", "arm64"):
        arch_str = "aarch64"
    else:
        raise RuntimeError(f"Unsupported architecture for mongot-extension: {arch}")

    # Detect platform. Default to amazon2023, use amazon2 only for Amazon Linux 2.
    platform_str = "amazon2023"
    if os.path.isfile("/etc/os-release"):
        with open("/etc/os-release") as f:
            os_release = f.read()
        # Check for Amazon Linux 2.
        if "ID=amzn" in os_release or 'ID="amzn"' in os_release:
            if 'VERSION_ID="2"' in os_release or "VERSION_ID=2\n" in os_release:
                platform_str = "amazon2"

    return f"https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-{platform_str}-{arch_str}.tgz"


def download_extension(so_path: str, logger: logging.Logger) -> None:
    """Download and install the mongot-extension from S3."""
    url = get_tarball_url()
    logger.info("Downloading mongot-extension from %s", url)

    with tempfile.TemporaryDirectory() as tmpdir:
        tarball_path = os.path.join(tmpdir, "mongot_extension.tgz")

        # Download the tarball.
        if not download_s3_binary(url, tarball_path):
            raise RuntimeError(f"Failed to download mongot-extension from {url}")

        # Extract the tarball.
        try:
            with tarfile.open(tarball_path, "r:gz") as tar:
                tar.extractall(path=tmpdir)
        except tarfile.TarError as e:
            raise RuntimeError(f"Failed to extract mongot-extension tarball: {e}")

        # Find the .so file in the extracted contents.
        extracted_so = os.path.join(tmpdir, SO_FILENAME)
        if not os.path.isfile(extracted_so):
            raise RuntimeError(f"Could not find {SO_FILENAME} in extracted tarball")

        # Create install directory and copy the .so file.
        os.makedirs(os.path.dirname(so_path), exist_ok=True)
        shutil.copy2(extracted_so, so_path)

    logger.info("Successfully installed mongot-extension to %s", so_path)


def setup_mongot_extension(
    is_evergreen: bool = False,
    logger: Optional[logging.Logger] = None,
) -> str:
    """Setup the mongot-extension by downloading the .so and creating the config file."""
    if logger is None:
        logger = logging.getLogger(__name__)

    so_path = get_so_path(is_evergreen)
    conf_path = os.path.join(CONF_OUT_DIR, CONF_FILENAME)

    # If both .so and config already exist, return early.
    if os.path.isfile(so_path) and os.path.isfile(conf_path):
        logger.info("mongot-extension already configured")
        return conf_path

    # Download the .so file.
    download_extension(so_path, logger)

    # Create config file.
    os.makedirs(CONF_OUT_DIR, exist_ok=True)
    with open(conf_path, "w") as f:
        f.write(f"sharedLibraryPath: {so_path}\n")
    logger.info("Created config file at %s", conf_path)

    return conf_path
