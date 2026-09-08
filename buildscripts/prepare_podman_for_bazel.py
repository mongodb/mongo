#!/usr/bin/env python3
"""Prepare the rootless Podman runtime used by local Bazel container actions."""

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import uuid
from collections.abc import Callable, Mapping, Sequence

_PODMAN_RUNTIME_DIR_PREFIX = "mongo-linux-podman-runtime-"
_PODMAN_STORAGE_DIR_PREFIX = "mongo-linux-podman-storage-"
_PODMAN_TASK_ID_ENV = "MONGO_PODMAN_TASK_ID"
_PODMAN_AUTH_FILE_ENV = "REGISTRY_AUTH_FILE"
_PODMAN_CONFIG_ENV = "CONTAINERS_CONF"
_COMMAND_TIMEOUT_SECONDS = 60
_CONTAINER_ACTIONS_DISABLED_VALUES = {"0", "false", "no", "off"}
_PODMAN_RECOVERY_MARKERS = (
    "invalid internal status",
    "conmon exited prematurely",
    "overlay",
    "mount",
    "permission denied",
)

Runner = Callable[..., subprocess.CompletedProcess[str]]
Which = Callable[[str], str | None]


def _run(argv: Sequence[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(argv), check=False, text=True, **kwargs)


def _command_name(command: str) -> str | None:
    try:
        argv = shlex.split(command)
    except ValueError:
        return None
    return pathlib.Path(argv[0]).name if argv else None


def _is_podman_docker_shim(command: str, runner: Runner = _run) -> bool:
    if _command_name(command) in {None, "podman"}:
        return False

    try:
        result = runner(
            [*shlex.split(command), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return False

    version_output = f"{result.stdout}\n{result.stderr}".lower()
    return "emulate docker cli using podman" in version_output or "podman version" in version_output


def podman_is_in_use(
    env: Mapping[str, str],
    *,
    system: str | None = None,
    which: Which = shutil.which,
    runner: Runner = _run,
) -> bool:
    """Match the Linux runtime selection used by hermetic_container_integration.py."""

    if (system or platform.system()) != "Linux":
        return False
    if env.get("MONGO_LINUX_CONTAINER_ACTIONS", "").lower() in _CONTAINER_ACTIONS_DISABLED_VALUES:
        return False

    podman = which("podman")
    if podman is None:
        return False

    explicit_runtime = env.get("HERMETIC_CONTAINER_DOCKER_COMMAND")
    if explicit_runtime:
        return _command_name(explicit_runtime) == "podman" or _is_podman_docker_shim(
            explicit_runtime, runner
        )

    docker = which("docker")
    if docker is None or _is_podman_docker_shim(docker, runner):
        return True

    # The Bazel wrapper prefers Docker when its daemon is healthy and falls back to
    # Podman when Docker is unavailable.
    try:
        docker_info = runner(
            [docker, "info"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return True
    return docker_info.returncode != 0


def _podman_task_root(env: Mapping[str, str], runtime_root: pathlib.Path | None) -> pathlib.Path:
    root = pathlib.Path("/tmp") if runtime_root is None else runtime_root
    task_id = env.get(_PODMAN_TASK_ID_ENV, "")
    if task_id:
        safe_task_id = re.sub(r"[^A-Za-z0-9_.-]", "_", task_id)
        root /= f"mongo-linux-podman-task-{safe_task_id}"
    return root


def _ensure_owned_directory(path: pathlib.Path, uid: int) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or (
        platform.system() == "Linux" and metadata.st_uid != uid
    ):
        raise OSError(f"Podman directory is not a directory owned by uid {uid}: {path}")
    path.chmod(0o700)


def _podman_storage_config(runtime_dir: pathlib.Path, uid: int) -> pathlib.Path:
    storage_dir = runtime_dir.parent / f"{_PODMAN_STORAGE_DIR_PREFIX}{uid}"
    graph_root = storage_dir / "graphroot"
    run_root = storage_dir / "runroot"
    _ensure_owned_directory(storage_dir, uid)
    _ensure_owned_directory(graph_root, uid)
    _ensure_owned_directory(run_root, uid)

    config_path = storage_dir / "storage.conf"
    content = "\n".join(
        [
            "[storage]",
            'driver = "overlay"',
            f"graphroot = {json.dumps(str(graph_root))}",
            f"runroot = {json.dumps(str(run_root))}",
            f"rootless_storage_path = {json.dumps(str(graph_root))}",
            "",
        ]
    )
    mount_program = shutil.which("fuse-overlayfs")
    if mount_program:
        content += "\n".join(
            [
                "[storage.options.overlay]",
                f"mount_program = {json.dumps(mount_program)}",
                "",
            ]
        )
    try:
        metadata = config_path.lstat()
    except FileNotFoundError:
        metadata = None
    if metadata is not None:
        if not stat.S_ISREG(metadata.st_mode) or (
            platform.system() == "Linux" and metadata.st_uid != uid
        ):
            raise OSError(
                "Podman storage configuration is not a file owned by " f"uid {uid}: {config_path}"
            )
        if config_path.read_text(encoding="utf-8") == content:
            config_path.chmod(0o600)
            return config_path

    temporary_path = config_path.with_name(f".{config_path.name}.{os.getpid()}.{uuid.uuid4().hex}")
    try:
        temporary_path.write_text(content, encoding="utf-8")
        temporary_path.chmod(0o600)
        temporary_path.replace(config_path)
    finally:
        temporary_path.unlink(missing_ok=True)
    return config_path


def _podman_containers_config(runtime_dir: pathlib.Path, uid: int) -> pathlib.Path:
    """Configure task-scoped rootless Podman for hosts without a user systemd session.

    Evergreen tasks do not normally have a systemd user session.  Podman otherwise
    probes the systemd cgroup manager on every invocation, emits a fallback warning,
    and then uses cgroupfs anyway.  The task runtime is ephemeral and does not need
    systemd delegation, so make the effective fallback explicit.
    """

    config_path = runtime_dir / "containers.conf"
    content = '[engine]\ncgroup_manager = "cgroupfs"\n'
    try:
        metadata = config_path.lstat()
    except FileNotFoundError:
        metadata = None
    if metadata is not None:
        if not stat.S_ISREG(metadata.st_mode) or (
            platform.system() == "Linux" and metadata.st_uid != uid
        ):
            raise OSError(
                "Podman configuration is not a file owned by " f"uid {uid}: {config_path}"
            )
        if config_path.read_text(encoding="utf-8") == content:
            config_path.chmod(0o600)
            return config_path

    temporary_path = config_path.with_name(f".{config_path.name}.{os.getpid()}.{uuid.uuid4().hex}")
    try:
        temporary_path.write_text(content, encoding="utf-8")
        temporary_path.chmod(0o600)
        temporary_path.replace(config_path)
    finally:
        temporary_path.unlink(missing_ok=True)
    return config_path


def _podman_auth_file(env: Mapping[str, str]) -> pathlib.Path | None:
    """Locate host registry credentials before task-scoping ``XDG_RUNTIME_DIR``."""

    explicit = env.get(_PODMAN_AUTH_FILE_ENV)
    if explicit:
        return pathlib.Path(explicit) if pathlib.Path(explicit).is_file() else None

    candidates: list[pathlib.Path] = []
    if env.get("XDG_RUNTIME_DIR"):
        candidates.append(pathlib.Path(env["XDG_RUNTIME_DIR"]) / "containers" / "auth.json")
    if env.get("XDG_CONFIG_HOME"):
        candidates.append(pathlib.Path(env["XDG_CONFIG_HOME"]) / "containers" / "auth.json")
    if env.get("DOCKER_CONFIG"):
        candidates.append(pathlib.Path(env["DOCKER_CONFIG"]) / "config.json")
    if env.get("HOME"):
        home = pathlib.Path(env["HOME"])
        candidates.extend(
            [home / ".config" / "containers" / "auth.json", home / ".docker" / "config.json"]
        )

    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _podman_runtime_env(
    env: Mapping[str, str],
    *,
    uid: int,
    runtime_root: pathlib.Path | None,
) -> dict[str, str]:
    task_root = _podman_task_root(env, runtime_root)
    runtime_dir = task_root / f"{_PODMAN_RUNTIME_DIR_PREFIX}{uid}"
    _ensure_owned_directory(runtime_dir, uid)
    storage_config = _podman_storage_config(runtime_dir, uid)
    containers_config = env.get(_PODMAN_CONFIG_ENV) or str(
        _podman_containers_config(runtime_dir, uid)
    )

    runtime_env = dict(env)
    runtime_env.update(
        {
            "CONTAINERS_STORAGE_CONF": str(storage_config),
            "XDG_RUNTIME_DIR": str(runtime_dir),
            "TMPDIR": str(runtime_dir),
            "TMP": str(runtime_dir),
            "TEMP": str(runtime_dir),
            _PODMAN_CONFIG_ENV: containers_config,
        }
    )
    auth_file = _podman_auth_file(env)
    if auth_file is not None:
        runtime_env[_PODMAN_AUTH_FILE_ENV] = str(auth_file)
    return runtime_env


def _emit(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)


def _podman_task_overlay(task_root: pathlib.Path, uid: int) -> pathlib.Path:
    # Rootless Podman mounts container writable layers below
    # ``overlay-containers/<id>/userdata/overlay``.  Unmount that parent rather
    # than ``graphroot/overlay`` (which is only the layer store and misses the
    # mount that Podman itself fails to clean up).
    return task_root / f"{_PODMAN_STORAGE_DIR_PREFIX}{uid}" / "graphroot" / "overlay-containers"


def _podman_result_detail(result: subprocess.CompletedProcess[str]) -> str:
    return (result.stderr or result.stdout or "").strip()


def _podman_failure_needs_mount_recovery(result: subprocess.CompletedProcess[str]) -> bool:
    # A negative return code means the Podman process was terminated by a signal
    # (the observed conmon/podman panic is SIGABRT, -6).  There is no useful
    # stderr in that case, but the task-scoped overlay still needs unmounting.
    if result.returncode < 0:
        return True
    detail = _podman_result_detail(result).casefold()
    return any(marker in detail for marker in _PODMAN_RECOVERY_MARKERS)


def _unmount_task_overlay(
    task_root: pathlib.Path,
    uid: int,
    runtime_env: Mapping[str, str],
    *,
    runner: Runner,
    force: bool = False,
) -> bool:
    """Best-effort unmount of this task's rootless overlay storage.

    Podman can leave a FUSE overlay mounted after conmon aborts.  Removing the
    task directory or running ``podman system reset`` then reports EPERM.  The
    path is derived solely from the task-scoped storage root; no shared Podman
    storage is ever passed to ``umount``.
    """

    overlay = _podman_task_overlay(task_root, uid)
    if not force and not overlay.exists():
        return True

    command = ["podman", "unshare", "umount", "-R", "-l", str(overlay)]
    try:
        result = runner(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=runtime_env,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"WARNING: Podman overlay unmount failed: {exc}", file=sys.stderr)
        return False

    # ``umount`` returns non-zero when no mount exists.  That is harmless during
    # best-effort teardown; retain diagnostics only for an actual failure.
    if result.returncode != 0 and _podman_result_detail(result):
        print(
            "WARNING: Podman overlay unmount exited with "
            f"{result.returncode}: {_podman_result_detail(result)}",
            file=sys.stderr,
        )
    return result.returncode == 0


def _emit_podman_failure(command: Sequence[str], result: subprocess.CompletedProcess[str]) -> None:
    """Report a bounded cleanup error without replaying a Podman panic trace."""

    detail = _podman_result_detail(result)
    if result.returncode < 0 or "panic:" in detail.casefold():
        detail = "Podman terminated while cleaning the task runtime"
    elif len(detail) > 1000:
        detail = detail[:500] + " ... " + detail[-500:]
    suffix = f": {detail}" if detail else ""
    print(
        f"WARNING: Podman cleanup command exited with {result.returncode}: "
        f"{' '.join(command)}{suffix}",
        file=sys.stderr,
    )


def prepare_podman_for_bazel(
    env: Mapping[str, str] | None = None,
    *,
    runner: Runner = _run,
    system: str | None = None,
    which: Which = shutil.which,
    uid: int | None = None,
    runtime_root: pathlib.Path | None = None,
) -> int:
    current_env = dict(env or os.environ)
    if not podman_is_in_use(current_env, system=system, which=which, runner=runner):
        print("Podman is not the selected Bazel container runtime; no preparation needed.")
        return 0

    podman_uid = os.getuid() if uid is None else uid
    runtime_env = _podman_runtime_env(current_env, uid=podman_uid, runtime_root=runtime_root)
    print(f"Preparing Podman for Bazel container actions for uid {podman_uid}.")
    print(f"Using task-scoped Podman storage: {runtime_env['CONTAINERS_STORAGE_CONF']}.")

    try:
        linger = runner(
            ["sudo", "loginctl", "enable-linger", str(podman_uid)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"WARNING: Could not enable systemd user lingering: {exc}", file=sys.stderr)
        linger = None
    if linger is not None:
        _emit(linger)
    if linger is None or linger.returncode != 0:
        print(
            "WARNING: Could not enable systemd user lingering; continuing with Podman cleanup.",
            file=sys.stderr,
        )

    for command in (
        ["podman", "system", "migrate"],
        ["podman", "rm", "--all", "--force"],
        ["podman", "system", "reset", "--force"],
        ["podman", "info"],
    ):
        try:
            result = runner(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=runtime_env,
                timeout=_COMMAND_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            print(f"WARNING: Podman command failed: {' '.join(command)}: {exc}", file=sys.stderr)
            if command in (
                ["podman", "system", "migrate"],
                ["podman", "rm", "--all", "--force"],
            ):
                continue
            return 1
        if result.returncode == 0:
            _emit(result)
        if result.returncode != 0:
            if _podman_failure_needs_mount_recovery(result):
                _unmount_task_overlay(
                    _podman_task_root(current_env, runtime_root),
                    podman_uid,
                    runtime_env,
                    runner=runner,
                    force=True,
                )
            if command in (
                ["podman", "system", "migrate"],
                ["podman", "rm", "--all", "--force"],
            ):
                _emit_podman_failure(command, result)
                continue
            if command == ["podman", "system", "reset", "--force"]:
                # Reset can race an overlay teardown.  Once the private overlay
                # is detached, one retry is enough to clear the task state.
                try:
                    retry = runner(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        env=runtime_env,
                        timeout=_COMMAND_TIMEOUT_SECONDS,
                    )
                except (OSError, subprocess.TimeoutExpired) as exc:
                    print(f"WARNING: Podman reset retry failed: {exc}", file=sys.stderr)
                else:
                    if retry.returncode == 0:
                        _emit(retry)
                        continue
                    _emit_podman_failure(command, retry)
            return result.returncode

    return 0


def cleanup_podman_for_bazel(
    env: Mapping[str, str] | None = None,
    *,
    runner: Runner = _run,
    system: str | None = None,
    which: Which = shutil.which,
    uid: int | None = None,
    runtime_root: pathlib.Path | None = None,
) -> int:
    """Remove all task-owned Podman state without masking the build result.

    Evergreen hosts are reused between tasks.  The runtime and storage paths are
    task-scoped, so resetting this Podman context cannot affect another task or a
    user's normal Podman containers.  Cleanup is deliberately best effort: a
    failed teardown must not replace the compile/test failure that caused it.
    """

    current_env = dict(env or os.environ)
    task_id = current_env.get(_PODMAN_TASK_ID_ENV, "")
    if not task_id:
        print("No task-scoped Podman id was provided; no cleanup needed.")
        return 0
    task_root = _podman_task_root(current_env, runtime_root)
    if not task_root.exists():
        print(f"Task-scoped Podman directory does not exist: {task_root}")
        return 0

    if not podman_is_in_use(current_env, system=system, which=which, runner=runner):
        # A later task may select Docker after an earlier Podman task failed. The
        # task directory is still ours to remove even when no Podman executable is
        # available for a final system reset.
        print("Podman is not the selected Bazel container runtime; removing stale task state.")
        try:
            shutil.rmtree(task_root)
        except OSError as exc:
            # Docker may be selected after a failed Podman preflight, while the
            # old task's rootless FUSE mounts remain.  If Podman is still
            # installed, detach only this task's overlay before retrying.  A
            # missing Podman binary leaves the original warning intact.
            podman_uid = os.getuid() if uid is None else uid
            podman_command = which("podman")
            if podman_command:
                try:
                    recovery_env = _podman_runtime_env(
                        current_env, uid=podman_uid, runtime_root=runtime_root
                    )
                    _unmount_task_overlay(
                        task_root,
                        podman_uid,
                        recovery_env,
                        runner=runner,
                        force=True,
                    )
                    shutil.rmtree(task_root)
                    return 0
                except (OSError, subprocess.TimeoutExpired) as recovery_exc:
                    print(
                        f"WARNING: Could not recover stale Podman mounts: {recovery_exc}",
                        file=sys.stderr,
                    )
            print(
                f"WARNING: Could not remove Podman task directory {task_root}: {exc}",
                file=sys.stderr,
            )
        return 0

    podman_uid = os.getuid() if uid is None else uid
    try:
        runtime_env = _podman_runtime_env(current_env, uid=podman_uid, runtime_root=runtime_root)
    except OSError as exc:
        print(f"WARNING: Could not prepare Podman cleanup environment: {exc}", file=sys.stderr)
        return 0

    print(f"Cleaning task-scoped Podman state for uid {podman_uid}.")
    # A rootless pause process can become stale while a task is running. In that
    # state even ``podman rm --all --force`` fails with "invalid internal status".
    # Migrate this isolated task-scoped runtime first so the container and overlay
    # mounts can be torn down before resetting its storage.
    task_root = _podman_task_root(current_env, runtime_root)
    for command in (
        ["podman", "system", "migrate"],
        ["podman", "rm", "--all", "--force"],
        ["podman", "system", "reset", "--force"],
    ):
        try:
            result = runner(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=runtime_env,
                timeout=_COMMAND_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            print(f"WARNING: Podman cleanup command failed: {exc}", file=sys.stderr)
            continue
        if result.returncode == 0:
            _emit(result)
            continue

        if _podman_failure_needs_mount_recovery(result):
            _unmount_task_overlay(
                task_root,
                podman_uid,
                runtime_env,
                runner=runner,
                force=True,
            )

        _emit_podman_failure(command, result)
        if command == ["podman", "system", "reset", "--force"]:
            try:
                retry = runner(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=runtime_env,
                    timeout=_COMMAND_TIMEOUT_SECONDS,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                print(f"WARNING: Podman cleanup retry failed: {exc}", file=sys.stderr)
            else:
                if retry.returncode == 0:
                    _emit(retry)
                else:
                    _emit_podman_failure(command, retry)

    try:
        shutil.rmtree(task_root)
    except OSError:
        # Podman may leave a rootless overlay mount behind when its pause process
        # was stale. Unmount only this task's private graphroot, then retry removal;
        # never touch the host user's normal Podman storage.
        _unmount_task_overlay(
            task_root,
            podman_uid,
            runtime_env,
            runner=runner,
            force=True,
        )
        try:
            shutil.rmtree(task_root)
        except OSError as retry_exc:
            print(
                f"WARNING: Could not remove Podman task directory {task_root}: {retry_exc}",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    if "--cleanup" in sys.argv[1:]:
        sys.exit(cleanup_podman_for_bazel())
    sys.exit(prepare_podman_for_bazel())
