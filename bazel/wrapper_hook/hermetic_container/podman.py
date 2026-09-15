"""Podman runtime lifecycle: storage config, recovery, and locks."""

from __future__ import annotations

import contextlib
import json
import os
import pathlib
import shlex
import shutil
import stat
import subprocess
import sys
import uuid
from collections.abc import Mapping, Sequence

from .constants import DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS
from .distro import _safe_name
from .podman_common import (
    PODMAN_AUTH_FILE_ENV,
    PODMAN_CONFIG_ENV,
    PODMAN_RUNTIME_DIR_PREFIX,
    PODMAN_STORAGE_DIR_PREFIX,
    PODMAN_TASK_ID_ENV,
)

PODMAN_STALE_RUNTIME_MARKER = "invalid internal status"

PODMAN_REPOSITORY_ENV_VARS = (
    "CONTAINERS_STORAGE_CONF",
    PODMAN_CONFIG_ENV,
    "XDG_RUNTIME_DIR",
    "TMPDIR",
    "TMP",
    "TEMP",
    PODMAN_AUTH_FILE_ENV,
)


def _is_podman_command(command: Sequence[str]) -> bool:
    return any(pathlib.Path(part).name == "podman" for part in command)


def _podman_task_root(env: Mapping[str, str]) -> pathlib.Path:
    root = pathlib.Path("/tmp")
    task_id = env.get(PODMAN_TASK_ID_ENV, "")
    if task_id:
        root /= f"mongo-linux-podman-task-{_safe_name(task_id)}"
    return root


def _ensure_owned_podman_directory(path: pathlib.Path, uid: int) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != uid:
        raise OSError(f"Podman directory is not a directory owned by uid {uid}: {path}")
    path.chmod(0o700)


def _podman_storage_config(runtime_dir: pathlib.Path) -> pathlib.Path:
    uid = os.getuid()
    storage_dir = runtime_dir.parent / f"{PODMAN_STORAGE_DIR_PREFIX}{uid}"
    graph_root = storage_dir / "graphroot"
    run_root = storage_dir / "runroot"
    _ensure_owned_podman_directory(storage_dir, uid)
    _ensure_owned_podman_directory(graph_root, uid)
    _ensure_owned_podman_directory(run_root, uid)

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
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != uid:
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


def _podman_containers_config(runtime_dir: pathlib.Path) -> pathlib.Path:
    """Configure task-scoped rootless Podman for hosts without user systemd."""
    _ensure_owned_podman_directory(runtime_dir, os.getuid())
    config_path = runtime_dir / "containers.conf"
    content = '[engine]\ncgroup_manager = "cgroupfs"\n'
    uid = os.getuid()
    try:
        metadata = config_path.lstat()
    except FileNotFoundError:
        metadata = None
    if metadata is not None:
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != uid:
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
    """Find the host registry credentials before replacing Podman's runtime directory.

    Rootless Podman normally searches ``$XDG_RUNTIME_DIR/containers/auth.json``.
    Cross actions intentionally use a task-scoped runtime directory, so that
    default lookup would hide the credentials prepared on the Evergreen host.
    Keep an explicit auth file when supplied, then inspect the standard Docker
    and Podman locations without copying credential contents into the task
    workspace.
    """

    explicit = env.get(PODMAN_AUTH_FILE_ENV)
    if explicit:
        return pathlib.Path(explicit) if pathlib.Path(explicit).is_file() else None

    candidates: list[pathlib.Path] = []
    xdg_runtime = env.get("XDG_RUNTIME_DIR")
    if xdg_runtime:
        candidates.append(pathlib.Path(xdg_runtime) / "containers" / "auth.json")
    xdg_config = env.get("XDG_CONFIG_HOME")
    if xdg_config:
        candidates.append(pathlib.Path(xdg_config) / "containers" / "auth.json")
    docker_config = env.get("DOCKER_CONFIG")
    if docker_config:
        candidates.append(pathlib.Path(docker_config) / "config.json")
    home = env.get("HOME")
    if home:
        home_path = pathlib.Path(home)
        candidates.extend(
            [
                home_path / ".config" / "containers" / "auth.json",
                home_path / ".docker" / "config.json",
            ]
        )

    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def _podman_anonymous_runtime_env(runtime_env: Mapping[str, str]) -> dict[str, str]:
    """Return a task-scoped Podman environment that cannot reuse stale credentials.

    Some Evergreen images are public in Quay, but a host can retain an expired robot
    credential in ``$HOME/.docker/config.json``.  Podman reports that stale credential
    as an authentication failure instead of falling back to anonymous access.  Keep
    this fallback confined to the private task runtime so a retry never changes the
    host's login state or exposes credential contents.
    """
    anonymous_env = dict(runtime_env)
    runtime_dir = pathlib.Path(anonymous_env["XDG_RUNTIME_DIR"])
    anonymous_home = runtime_dir / "anonymous-home"
    anonymous_config = runtime_dir / "anonymous-config"
    anonymous_home.mkdir(mode=0o700, parents=True, exist_ok=True)
    anonymous_config.mkdir(mode=0o700, parents=True, exist_ok=True)
    anonymous_env["HOME"] = str(anonymous_home)
    anonymous_env["XDG_CONFIG_HOME"] = str(anonymous_config)
    anonymous_env.pop("DOCKER_CONFIG", None)
    anonymous_env.pop(PODMAN_AUTH_FILE_ENV, None)
    return anonymous_env


def _podman_authentication_failure(result: subprocess.CompletedProcess[str]) -> bool:
    """Return whether a pull failed because the configured registry login was rejected."""
    # Podman writes the high-level copy error to stderr, while the Docker
    # compatibility shim can put the registry response on stdout.  Inspect both
    # streams instead of selecting whichever one happens to be non-empty.
    detail = f"{result.stderr or ''}\n{result.stdout or ''}".casefold()
    return any(
        marker in detail
        for marker in (
            "invalid username/password",
            "could not find robot with specified username",
            "authentication required",
            "unauthorized",
        )
    )


def _podman_runtime_dir() -> pathlib.Path:
    """Return a private, task-scoped runtime directory outside the action workspace."""
    uid = os.getuid()
    path = _podman_task_root(os.environ) / f"{PODMAN_RUNTIME_DIR_PREFIX}{uid}"
    _ensure_owned_podman_directory(path, uid)
    return path


def _container_runtime_env(command: Sequence[str]) -> dict[str, str] | None:
    """Keep rootless Podman's runtime data out of an inherited task TMPDIR."""
    if not _is_podman_command(command):
        return None

    runtime_dir = _podman_runtime_dir()
    storage_config = _podman_storage_config(runtime_dir)
    containers_config = os.environ.get(PODMAN_CONFIG_ENV)
    if not containers_config and os.environ.get(PODMAN_TASK_ID_ENV, ""):
        # The cgroupfs override is only needed for task-scoped CI runtimes.
        # Without a task ID the runtime directory is shared, so leave Podman's
        # own defaults in place rather than writing config into shared state.
        containers_config = str(_podman_containers_config(runtime_dir))
    runtime_env = dict(os.environ)
    runtime_env.update(
        {
            "CONTAINERS_STORAGE_CONF": str(storage_config),
            "XDG_RUNTIME_DIR": str(runtime_dir),
            "TMPDIR": str(runtime_dir),
            "TMP": str(runtime_dir),
            "TEMP": str(runtime_dir),
        }
    )
    if containers_config:
        runtime_env[PODMAN_CONFIG_ENV] = containers_config
    auth_file = _podman_auth_file(env=os.environ)
    if auth_file is not None:
        runtime_env[PODMAN_AUTH_FILE_ENV] = str(auth_file)
    return runtime_env


def _compact_runtime_detail(detail: str, limit: int = 2000) -> str:
    """Bound noisy runtime failures while retaining their first and last causes."""
    detail = detail.strip()
    if len(detail) <= limit:
        return detail
    half = (limit - len("\n... output truncated ...\n")) // 2
    return detail[:half] + "\n... output truncated ...\n" + detail[-half:]


@contextlib.contextmanager
def _podman_recovery_lock(runtime_env: Mapping[str, str]):
    """Serialize recovery of the rootless Podman pause process."""
    import fcntl

    runtime_dir = pathlib.Path(runtime_env["XDG_RUNTIME_DIR"])
    lock_path = runtime_dir / "mongo-podman-recovery.lock"
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _podman_command_detail(result: subprocess.CompletedProcess[str]) -> str:
    return (result.stderr or result.stdout or "").strip()


def _print_container_command_output(result: subprocess.CompletedProcess[str]) -> None:
    """Keep pull diagnostics off stdout while preserving both output streams.

    Podman normally writes pull failures to stderr, but the Docker compatibility
    shim has emitted registry errors on stdout on some RHEL images.  Capturing
    both streams lets authentication-failure handling see the actual error while
    keeping Bazel's machine-readable stdout clean.
    """
    for output in (result.stdout, result.stderr):
        if output:
            print(output, file=sys.stderr, end="" if output.endswith("\n") else "\n")


def _podman_migration_is_allowed(env: Mapping[str, str]) -> bool:
    """Allow opting out of automatic `podman system migrate`."""
    return env.get("MONGO_BAZEL_PODMAN_AUTO_MIGRATE", "1").strip() not in ("0", "false", "False")


def _podman_migration_force_is_allowed(env: Mapping[str, str]) -> bool:
    """Allow forcing migration when Podman cannot enumerate containers."""
    return env.get("MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE", "0").strip().casefold() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _podman_runtime_is_task_scoped(runtime_env: Mapping[str, str]) -> bool:
    """Return whether a Podman runtime can be reset without touching shared state."""

    task_id = runtime_env.get(PODMAN_TASK_ID_ENV, "")
    storage_config = runtime_env.get("CONTAINERS_STORAGE_CONF", "")
    if not task_id or not storage_config:
        return False
    try:
        storage_path = pathlib.Path(storage_config).resolve()
        task_root = _podman_task_root(runtime_env).resolve()
        return task_root in storage_path.parents
    except OSError:
        return False


def _reset_task_scoped_podman_runtime(
    podman_argv: Sequence[str], runtime_env: Mapping[str, str]
) -> tuple[bool, str]:
    """Detach task-local overlays and reset state after a Podman panic."""

    if not _podman_runtime_is_task_scoped(runtime_env):
        return False, "Podman storage is not task-scoped"

    storage_config = pathlib.Path(runtime_env["CONTAINERS_STORAGE_CONF"])
    # Container writable layers are mounted below overlay-containers, not the
    # top-level overlay layer store.  Unmount this parent recursively so a
    # conmon abort cannot leave userdata/overlay mounted across task teardown.
    overlay = storage_config.parent / "graphroot" / "overlay-containers"
    unmount = subprocess.run(
        [*podman_argv, "unshare", "umount", "-R", "-l", str(overlay)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        env=runtime_env,
    )
    reset = subprocess.run(
        [*podman_argv, "system", "reset", "--force"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        env=runtime_env,
    )
    if reset.returncode != 0:
        return False, (
            "task-scoped Podman reset failed: "
            f"{_podman_command_detail(reset) or f'exit code {reset.returncode}'}"
        )
    if unmount.returncode != 0 and _podman_command_detail(unmount):
        # A missing mount is expected after a clean reset; retain diagnostics only
        # when unshare reported a real error and let the info retry decide health.
        return True, f"overlay unmount warning: {_podman_command_detail(unmount)}"
    return True, ""


def _podman_has_running_containers(
    podman_argv: Sequence[str],
    runtime_env: Mapping[str, str],
) -> tuple[bool | None, str]:
    """Report whether this user still has usable running containers.

    ``None`` means that Podman could not determine the answer. In particular,
    a stale pause process can make existing containers unreachable without
    stopping their workload processes, so a failed listing must not authorize
    a destructive migration by itself.
    """
    result = subprocess.run(
        [*podman_argv, "ps", "--quiet"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
        env=runtime_env,
    )
    if result.returncode != 0:
        return None, _podman_command_detail(result) or f"exit code {result.returncode}"
    return bool(result.stdout.strip()), ""


def _recover_stale_podman_runtime(
    info_argv: Sequence[str],
    runtime_env: Mapping[str, str],
) -> tuple[bool, str]:
    """Check whether another process repaired stale Podman state, under a lock."""
    try:
        with _podman_recovery_lock(runtime_env):
            # Another Bazel invocation may have recovered Podman while this one
            # waited for the lock, so check again before mutating runtime state.
            recheck = subprocess.run(
                info_argv,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if recheck.returncode == 0:
                return True, ""

            recheck_detail = _podman_command_detail(recheck)
            if PODMAN_STALE_RUNTIME_MARKER not in recheck_detail:
                return False, (
                    "Podman remained unavailable after acquiring the recovery lock: "
                    f"{recheck_detail or f'exit code {recheck.returncode}'}"
                )

            podman_argv = list(info_argv[:-1])
            if not _podman_migration_is_allowed(runtime_env):
                return False, (
                    "Automatic Podman runtime migration is disabled by "
                    "MONGO_BAZEL_PODMAN_AUTO_MIGRATE. Run `podman system migrate` manually "
                    "after verifying that stopping running containers is safe, then retry."
                )

            has_running_containers, container_check_detail = _podman_has_running_containers(
                podman_argv, runtime_env
            )
            task_scoped_runtime = _podman_runtime_is_task_scoped(runtime_env)
            reset_detail = ""
            # A listing that fails with the same stale-runtime signature is
            # evidence that the runtime is broken, not that containers are
            # reachable: every Podman command shares that state. Recover when the
            # runtime is private to this task, which is the case the guard below
            # is protecting against multi-tenant runtimes.
            listing_blocked_by_stale_runtime = (
                has_running_containers is None
                and PODMAN_STALE_RUNTIME_MARKER in container_check_detail
                and _podman_runtime_is_task_scoped(runtime_env)
            )
            if (
                has_running_containers is None
                and not listing_blocked_by_stale_runtime
                and not _podman_migration_force_is_allowed(runtime_env)
            ):
                return False, (
                    "Refusing to run `podman system migrate` because Podman could not "
                    "determine whether this user has running containers: "
                    f"{container_check_detail}. Set "
                    "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE=1 only when this Podman runtime "
                    "is isolated and stopping its containers is safe, then retry."
                )

            if has_running_containers:
                return False, (
                    "Refusing to run `podman system migrate` because it stops the containers "
                    "currently running for this user. Run `podman system migrate` manually "
                    "after verifying that stopping those containers is safe, then retry."
                )

            migrate = subprocess.run(
                [*podman_argv, "system", "migrate"],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if migrate.returncode != 0:
                if task_scoped_runtime:
                    reset, reset_detail = _reset_task_scoped_podman_runtime(
                        podman_argv, runtime_env
                    )
                    if reset:
                        retry = subprocess.run(
                            info_argv,
                            check=False,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True,
                            timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                            env=runtime_env,
                        )
                        if retry.returncode == 0:
                            return True, reset_detail
                migrate_detail = _compact_runtime_detail(_podman_command_detail(migrate))
                if task_scoped_runtime and reset_detail:
                    migrate_detail = f"{migrate_detail}; {reset_detail}".strip("; ")
                return False, (
                    "`podman system migrate` failed: "
                    f"{migrate_detail or f'exit code {migrate.returncode}'}"
                )

            retry = subprocess.run(
                info_argv,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS,
                env=runtime_env,
            )
            if retry.returncode == 0:
                return True, ""

            retry_detail = _podman_command_detail(retry)
            return False, (
                "Podman remained unavailable after `podman system migrate`: "
                f"{retry_detail or f'exit code {retry.returncode}'}"
            )
    except FileNotFoundError as exc:
        return False, f"Podman recovery command not found: {exc.filename}"
    except subprocess.TimeoutExpired as exc:
        command = " ".join(shlex.quote(str(part)) for part in exc.cmd)
        return False, (
            f"{command} timed out after {DOCKER_DAEMON_CHECK_TIMEOUT_SECONDS}s "
            "during stale Podman runtime recovery"
        )
    except OSError as exc:
        return False, f"could not recover stale Podman runtime state: {exc}"
