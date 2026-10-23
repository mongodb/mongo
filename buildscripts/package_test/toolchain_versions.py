"""Validate the tool versions recorded in build provenance."""

import json
import shlex
from pathlib import Path
from typing import Any


def validate_toolchain_versions(
    build_environment: dict[str, Any], expected_versions: dict | None = None
) -> None:
    if expected_versions is None:
        expected_versions = json.loads(Path(__file__).with_suffix(".json").read_text())
    platform = "/".join(build_environment.get(key, "") for key in ("target_os", "target_arch"))
    expected = expected_versions.get(platform)
    if expected is None:
        raise RuntimeError(f"No production toolchain versions are pinned for {platform!r}")
    for key in ("compiler", "compiler_version", "linker", "linker_version"):
        version = expected[key]
        actual = build_environment.get(key)
        if actual != version:
            raise RuntimeError(
                f"Production toolchain mismatch for {platform}: {key} expected {version!r}, "
                f"got {actual!r}"
            )


def should_require_production_toolchain(environment: dict[str, str]) -> bool:
    """Match the release profiles selected by maybe_release_flag for production tasks."""
    flags = shlex.split(
        " ".join(
            environment.get(key, "")
            for key in (
                "bazel_args",
                "bazel_compile_flags",
                "task_compile_flags",
                "patch_compile_flags",
            )
        )
    )
    for index, flag in enumerate(flags):
        config = flag.removeprefix("--config=")
        if flag == "--config" and index + 1 < len(flags):
            config = flags[index + 1]
        if config in {"public-release", "public-release-local", "public-release-rbe"}:
            return True
        if flag == "--define=MONGO_PRODUCTION_RELEASE=1":
            return True
    if environment.get("MONGO_VERSION_OVERRIDE"):
        return True
    if environment.get("compiling_for_test") == "true":
        return False
    if (
        environment.get("project") == "mongodb-mongo-master"
        and environment.get("is_release") != "true"
    ):
        return False
    if environment.get("release_rbe") == "true":
        return True
    return environment.get("is_patch") != "true" and bool(environment.get("push_bucket"))
