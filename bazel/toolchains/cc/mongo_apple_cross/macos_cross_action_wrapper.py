#!/usr/bin/env python3
"""Runs macOS cross-build actions inside a persistent Linux container."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import platform
import re
import shlex
import subprocess
import sys
from collections.abc import Sequence

try:
    import fcntl
except ImportError:
    fcntl = None

try:
    import msvcrt
except ImportError:
    msvcrt = None

DISABLED_VALUES = {"0", "false", "no", "off"}
DEFAULT_CONTAINER_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
PATH_ENV_VARS = ["LLVM_PATH", "MACOS_SDK_PATH"]
ACTION_CONFIG_FILENAME = "mongo_cross_host_action.json"


def _env_is_false(value: str | None) -> bool:
    return value is not None and value.lower() in DISABLED_VALUES


def _docker_command() -> list[str]:
    return shlex.split(os.environ.get("MONGO_MACOS_CROSS_ACTION_DOCKER_COMMAND", "docker"))


def _run(args: Sequence[str], *, stdout=None, stderr=None) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=False, stdout=stdout, stderr=stderr, text=True)


def _check(args: Sequence[str]) -> None:
    result = _run(args)
    if result.returncode:
        raise SystemExit(result.returncode)


def _output_base(path: pathlib.Path) -> pathlib.Path:
    resolved = path.resolve()
    for candidate in [resolved, *resolved.parents]:
        if candidate.name == "execroot":
            return candidate.parent
    raise RuntimeError(f"Could not infer Bazel output base from {path}")


def _execroot(path: pathlib.Path) -> pathlib.Path:
    resolved = path.resolve()
    for candidate in [resolved, *resolved.parents]:
        if candidate.parent.name == "execroot":
            return candidate
    raise RuntimeError(f"Could not infer Bazel execroot from {path}")


def _load_action_config(output_base: pathlib.Path) -> None:
    """Loads host-only wrapper settings written by the Bazel wrapper hook."""
    config_path = output_base / ACTION_CONFIG_FILENAME
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"Could not read cross action wrapper config {config_path}: {exc}"
        ) from exc

    if (
        not isinstance(config, dict)
        or config.get("version") != 1
        or not isinstance(config.get("environment"), dict)
    ):
        raise RuntimeError(f"Invalid cross action wrapper config: {config_path}")
    for key, value in config["environment"].items():
        if isinstance(key, str) and isinstance(value, str):
            os.environ[key] = value


def _safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def _container_execroot(repo_root: pathlib.Path, execroot: pathlib.Path) -> pathlib.Path:
    digest = hashlib.sha256(str(execroot).encode()).hexdigest()[:12]
    mirror = repo_root / ".tmp" / "hermetic_container" / "container-execroots" / digest
    mirror.mkdir(parents=True, exist_ok=True)

    for entry in execroot.iterdir():
        destination = mirror / entry.name
        if destination.exists() or destination.is_symlink():
            continue

        if entry.is_symlink():
            target = os.readlink(entry)
        else:
            target = str(entry)
        try:
            destination.symlink_to(target)
        except FileExistsError:
            pass

    return mirror


def _container_name(repo_root: pathlib.Path, output_base: pathlib.Path, image: str) -> str:
    prefix = os.environ.get(
        "MONGO_MACOS_CROSS_ACTION_CONTAINER_PREFIX",
        "mongo_macos_cross_action",
    )
    digest = hashlib.sha256(
        f"{repo_root.resolve()}|{output_base.resolve()}|{image}".encode()
    ).hexdigest()[:12]
    return _safe_name(f"{prefix}_{digest}")[:120]


def _image_exists(docker: Sequence[str], image: str) -> bool:
    return (
        _run(
            [*docker, "image", "inspect", image],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def _container_running(docker: Sequence[str], name: str) -> bool:
    result = _run(
        [*docker, "inspect", "-f", "{{.State.Running}}", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0 and result.stdout.strip() == "true"


def _container_exists(docker: Sequence[str], name: str) -> bool:
    return (
        _run(
            [*docker, "inspect", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ).returncode
        == 0
    )


def _volume(path: pathlib.Path, read_only: bool = False) -> list[str]:
    resolved = path.resolve()
    suffix = ":ro" if read_only else ""
    return ["-v", f"{resolved}:{resolved}{suffix}"]


def _extra_path_mounts() -> list[str]:
    mounts: list[str] = []
    for var_name in PATH_ENV_VARS:
        value = os.environ.get(var_name)
        if value:
            path = pathlib.Path(value)
            if path.exists():
                mounts.extend(_volume(path, read_only=True))
    return mounts


@contextlib.contextmanager
def _exclusive_file_lock(lock_path: pathlib.Path):
    """Acquire a process lock for container startup on Unix and Windows."""

    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock_file:
        if fcntl is not None:
            fcntl.flock(lock_file, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file, fcntl.LOCK_UN)
            return

        if msvcrt is not None:
            lock_file.seek(0, os.SEEK_END)
            if lock_file.tell() == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            return

        yield


def _start_container(
    docker: Sequence[str],
    *,
    name: str,
    image: str,
    dockerfile: str,
    repo_root: pathlib.Path,
    output_base: pathlib.Path,
) -> None:
    if dockerfile and not _image_exists(docker, image):
        _check([*docker, "build", "-t", image, "-f", dockerfile, str(repo_root)])

    if _container_running(docker, name):
        return

    if _container_exists(docker, name):
        _check([*docker, "rm", name])

    home = pathlib.Path(
        os.environ.get(
            "MONGO_MACOS_CROSS_ACTION_HOME",
            str(repo_root / ".tmp" / "hermetic_container" / "home"),
        )
    )
    home.mkdir(parents=True, exist_ok=True)

    command = [
        *docker,
        "run",
        "-d",
        "--name",
        name,
        "-w",
        str(repo_root),
        "-e",
        f"HOME={home}",
        "-e",
        f"PATH={DEFAULT_CONTAINER_PATH}",
        *_volume(repo_root),
        *_volume(output_base),
        *_volume(home),
        *_extra_path_mounts(),
    ]

    platform = os.environ.get("MONGO_MACOS_CROSS_ACTION_PLATFORM")
    if platform:
        command.extend(["--platform", platform])

    network = os.environ.get("MONGO_MACOS_CROSS_ACTION_NETWORK")
    if network:
        command.extend(["--network", network])

    user = os.environ.get("MONGO_MACOS_CROSS_ACTION_USER")
    if user:
        command.extend(["-u", user])

    command.extend(
        [image, "/bin/bash", "-lc", "trap 'exit 0' TERM INT; while true; do sleep 3600; done"]
    )
    _check(command)


def _exec_env(cwd: pathlib.Path) -> list[str]:
    env_args = ["-e", f"PATH={DEFAULT_CONTAINER_PATH}", "-e", f"PWD={cwd}"]
    for key, value in os.environ.items():
        if key in {"PATH", "PWD", "TMPDIR", "TMP", "TEMP"}:
            continue
        if "\x00" in key or "\x00" in value:
            continue
        env_args.extend(["-e", f"{key}={value}"])
    return env_args


def _run_in_container(real_tool: pathlib.Path, tool_args: Sequence[str]) -> int:
    cwd = pathlib.Path.cwd().resolve()
    output_base = _output_base(cwd)
    _load_action_config(output_base)
    repo_root = pathlib.Path(os.environ["MONGO_MACOS_CROSS_ACTION_REPO_ROOT"]).resolve()
    execroot = _execroot(cwd)
    mirror_execroot = _container_execroot(repo_root, execroot)
    mirror_cwd = mirror_execroot / cwd.relative_to(execroot)
    image = os.environ["MONGO_MACOS_CROSS_ACTION_IMAGE"]
    dockerfile = os.environ.get("MONGO_MACOS_CROSS_ACTION_DOCKERFILE", "")
    docker = _docker_command()
    name = _container_name(repo_root, output_base, image)

    lock_path = repo_root / ".tmp" / "hermetic_container" / "macos-cross-action-wrapper.lock"
    with _exclusive_file_lock(lock_path):
        _start_container(
            docker,
            name=name,
            image=image,
            dockerfile=dockerfile,
            repo_root=repo_root,
            output_base=output_base,
        )

    result = _run(
        [
            *docker,
            "exec",
            "-i",
            "-w",
            str(mirror_cwd),
            *_exec_env(mirror_cwd),
            name,
            str(real_tool.resolve()),
            *tool_args,
        ]
    )
    return result.returncode


def main(argv: Sequence[str]) -> int:
    if len(argv) < 2:
        print("usage: macos_cross_action_wrapper.py REAL_TOOL [ARGS...]", file=sys.stderr)
        return 2

    real_tool = pathlib.Path(argv[1])
    tool_args = argv[2:]

    if _env_is_false(os.environ.get("MONGO_MACOS_CROSS_ACTION_WRAPPER")):
        return _run([str(real_tool), *tool_args]).returncode

    if platform.system() == "Linux":
        return _run([str(real_tool), *tool_args]).returncode

    missing = [
        name
        for name in [
            "MONGO_MACOS_CROSS_ACTION_IMAGE",
            "MONGO_MACOS_CROSS_ACTION_REPO_ROOT",
        ]
        if not os.environ.get(name)
    ]
    if missing:
        print(
            "missing macOS cross action wrapper environment: " + ", ".join(missing),
            file=sys.stderr,
        )
        return 127

    return _run_in_container(real_tool, tool_args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
