#!/usr/bin/env python3
"""Runs Windows cross-build actions through the shared container wrapper."""

from __future__ import annotations

import os
import pathlib
import runpy
import sys

ENV_MAP = {
    "MONGO_WINDOWS_CROSS_ACTION_WRAPPER": "MONGO_MACOS_CROSS_ACTION_WRAPPER",
    "MONGO_WINDOWS_CROSS_ACTION_CONTAINER_PREFIX": "MONGO_MACOS_CROSS_ACTION_CONTAINER_PREFIX",
    "MONGO_WINDOWS_CROSS_ACTION_DOCKER_COMMAND": "MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND",
    "MONGO_WINDOWS_CROSS_ACTION_IMAGE": "MONGO_MACOS_CROSS_ACTION_IMAGE",
    "MONGO_WINDOWS_CROSS_ACTION_DOCKERFILE": "MONGO_MACOS_CROSS_ACTION_DOCKERFILE",
    "MONGO_WINDOWS_CROSS_ACTION_REPO_ROOT": "MONGO_MACOS_CROSS_ACTION_REPO_ROOT",
    "MONGO_WINDOWS_CROSS_ACTION_HOME": "MONGO_MACOS_CROSS_ACTION_HOME",
    "MONGO_WINDOWS_CROSS_ACTION_NETWORK": "MONGO_MACOS_CROSS_ACTION_NETWORK",
    "MONGO_WINDOWS_CROSS_ACTION_USER": "MONGO_MACOS_CROSS_ACTION_USER",
    "MONGO_WINDOWS_CROSS_ACTION_PLATFORM": "MONGO_MACOS_CROSS_ACTION_PLATFORM",
}

PATH_ENV_MAP = {
    "MONGO_WINDOWS_CROSS_LLVM_PATH": "LLVM_PATH",
    "MONGO_WINDOWS_CROSS_SYSROOT_PATH": "MACOS_SDK_PATH",
}


def main() -> int:
    for source, destination in ENV_MAP.items():
        if os.environ.get(source) and not os.environ.get(destination):
            os.environ[destination] = os.environ[source]

    for source, destination in PATH_ENV_MAP.items():
        if os.environ.get(source) and not os.environ.get(destination):
            os.environ[destination] = os.environ[source]

    script = (
        pathlib.Path(__file__).resolve().parents[1]
        / "mongo_apple_cross"
        / "macos_cross_action_wrapper.py"
    )
    runpy.run_path(str(script), run_name="__main__")
    return 0


if __name__ == "__main__":
    sys.exit(main())
