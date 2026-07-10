"""Download externally-published extensions (see etc/extensions.yml) for resmoke runs.

Mirrors src/mongo/db/extension/extensions/download_external_extension.py, the bazel-side
downloader.
"""

import hashlib
import io
import logging
import os
import platform
import shutil
import tarfile
import time

import requests
import yaml

from buildscripts.resmokelib.extensions.constants import (
    EXTERNAL_EXTENSIONS_CACHE_DIR,
    EXTERNAL_EXTENSIONS_CONF_PATH,
)

MAX_DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_TIMEOUT_SECS = 120


def download_external_extension(name: str, logger: logging.Logger) -> str | None:
    """Download lib<name>_extension.so (and its .sig) into the local cache; return the .so path.

    Returns None when <name> has no entry in etc/extensions.yml or no artifact for this
    architecture. Artifacts are only published for amazon2023; older-glibc distros will fail to
    dlopen the result.
    """
    conf_key = f"{name}_extension"
    try:
        with open(EXTERNAL_EXTENSIONS_CONF_PATH, "r") as f:
            entry = ((yaml.safe_load(f) or {}).get("extensions") or {}).get(conf_key)
    except FileNotFoundError:
        logger.warning(
            "Cannot download extension '%s': %s does not exist (cwd: %s).",
            name,
            EXTERNAL_EXTENSIONS_CONF_PATH,
            os.getcwd(),
        )
        return None
    if not entry:
        logger.info(
            "Extension '%s' has no '%s' entry in %s; not an external extension.",
            name,
            conf_key,
            EXTERNAL_EXTENSIONS_CONF_PATH,
        )
        return None

    variant = f"amazon2023-{platform.machine()}"
    checksum = (entry.get("variants") or {}).get(variant)
    if not checksum:
        logger.warning(
            "Extension '%s' has no published artifact for variant %s; not downloading.",
            name,
            variant,
        )
        return None

    # Versioned so a bump in etc/extensions.yml invalidates the cache.
    cache_dir = os.path.join(EXTERNAL_EXTENSIONS_CACHE_DIR, f"{entry['version']}-{variant}")
    so_path = os.path.join(cache_dir, f"lib{name}_extension.so")
    if os.path.exists(so_path) and os.path.exists(f"{so_path}.sig"):
        logger.info("Using cached external extension for '%s': %s", name, so_path)
        return so_path

    url = f"{entry['base_url']}{entry['name']}-{entry['version']}-{variant}.tgz"
    logger.info("Extension '%s' not found locally; downloading %s", name, url)
    last_exc: Exception | None = None
    for attempt in range(MAX_DOWNLOAD_ATTEMPTS):
        try:
            response = requests.get(url, timeout=DOWNLOAD_TIMEOUT_SECS)
            response.raise_for_status()
            break
        except requests.RequestException as e:
            last_exc = e
            logger.warning("Download attempt %d for %s failed: %s", attempt + 1, url, e)
            time.sleep(2**attempt)
    else:
        raise RuntimeError(f"Failed to download extension '{name}' from {url}") from last_exc
    if hashlib.sha256(response.content).hexdigest() != checksum:
        raise RuntimeError(f"Checksum mismatch downloading extension '{name}' from {url}")

    os.makedirs(cache_dir, exist_ok=True)
    # Extract to temp names and rename into place so an interrupted run can't leave a truncated
    # file that a later run's cache-hit check would trust. The .so is renamed last: its existence
    # is the cache-hit marker, so the .sig must already be in place by then.
    # Tarball members (<name>-extension.so[.sig]) map to the convention resmoke resolves.
    wanted = {
        f"{entry['name']}.so": so_path,
        f"{entry['name']}.so.sig": f"{so_path}.sig",
    }
    extracted: dict[str, str] = {}
    with tarfile.open(fileobj=io.BytesIO(response.content), mode="r:gz") as tarball:
        for member in tarball.getmembers():
            out_path = wanted.get(os.path.basename(member.name))
            if out_path is None:
                continue
            src = tarball.extractfile(member)
            if src is None:  # Non-file member (directory, link, etc.).
                continue
            tmp_path = f"{out_path}.tmp.{os.getpid()}"
            with src, open(tmp_path, "wb") as dst:
                shutil.copyfileobj(src, dst)
            extracted[out_path] = tmp_path

    if so_path not in extracted:
        for tmp_path in extracted.values():
            os.remove(tmp_path)
        raise RuntimeError(
            f"Downloaded archive for extension '{name}' from {url} did not contain "
            f"{entry['name']}.so"
        )
    for out_path in sorted(extracted, key=lambda p: p == so_path):
        os.rename(extracted[out_path], out_path)
    logger.info("Downloaded external extension for '%s' to %s", name, so_path)
    return so_path
