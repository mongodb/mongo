"""Read the target sysroot pins shared by publication and PR automation."""

import json
import re
from typing import Any

TOOLCHAIN_SYSROOT_DISTROS = ("rhel8", "rhel9", "rhel10")
TOOLCHAIN_SYSROOT_ARCHITECTURES = ("amd64", "arm64", "ppc64le", "s390x")


def read_sysroot_manifest(
    content: str, *, source: str = "Sysroot pin manifest", allow_empty: bool = True
) -> dict[str, Any]:
    """Validate nested pins, allowing an absent file on an older Git ref."""
    if not content and allow_empty:
        return {"schema-version": 1, "sysroots": {}}
    try:
        manifest = json.loads(content)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{source}: invalid JSON: {exc}") from exc
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema-version") != 1
        or not isinstance(manifest.get("sysroots"), dict)
    ):
        raise ValueError(f"{source}: expected a schema-version 1 sysroot pin manifest")
    for distro, pins in manifest["sysroots"].items():
        if distro not in TOOLCHAIN_SYSROOT_DISTROS:
            raise ValueError(f"{source}: Unsupported sysroot distro {distro!r}")
        field = f"sysroots.{distro}"
        if not isinstance(pins, dict):
            raise ValueError(f"{source}: {field} must be an architecture-to-pin object")
        for architecture, pin in pins.items():
            if architecture not in TOOLCHAIN_SYSROOT_ARCHITECTURES:
                raise ValueError(f"{source}: Unsupported sysroot architecture {architecture!r}")
            field = f"sysroots.{distro}.{architecture}"
            if not isinstance(pin, dict):
                raise ValueError(f"{source}: {field} must be a pin object")
            digest = pin.get("sha256")
            if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
                raise ValueError(
                    f"{source}: {field}.sha256 must be 64 lowercase hexadecimal digits"
                )
            filename = f"toolchain-sysroot-{distro}-{architecture}-{digest}.tar.gz"
            url = pin.get("url")
            # Match the HTTPS S3 URLs produced by the publisher, permitting custom
            # buckets/prefixes while excluding credentials, queries and Markdown.
            match = (
                re.fullmatch(
                    r"https://[a-z0-9][a-z0-9.-]*\.s3\.amazonaws\.com/([A-Za-z0-9._/-]+)",
                    url,
                )
                if isinstance(url, str)
                else None
            )
            components = match[1].split("/") if match else []
            if (
                not components
                or components[-1] != filename
                or any(component in ("", ".", "..") for component in components)
            ):
                raise ValueError(f"{source}: {field}.url must be an HTTPS S3 URL for {filename}")
    return manifest
