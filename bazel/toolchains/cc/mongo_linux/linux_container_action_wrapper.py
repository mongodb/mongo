#!/usr/bin/env python3
"""Runs Linux host build actions inside a persistent local build container.

Invoked by Bazel's persistent-container spawn strategy:

    linux_container_action_wrapper.py CONFIG_JSON REAL_TOOL [ARGS...]

CONFIG_JSON is written to the Bazel output base by bazel/wrapper_hook/hermetic_container_integration.py
before the build starts. Preflight snapshots it into host-only state; action invocations never trust
the copy exposed below the container-writable output base. Its presence routes a tool invocation.
Remote execution workers and hosts that opt out do not invoke this script; it only runs for
local actions selected by the persistent-container strategy.

The container is started from the same pinned image the host platform advertises for
remote execution, so containerized local results match remote results exactly — a
requirement for racing local and remote branches with Bazel dynamic scheduling. Because
dynamic scheduling cancels the losing branch, this wrapper forwards SIGTERM/SIGINT to the
process inside the container instead of orphaning it.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Mapping, Sequence


def _run(args: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    if "env" not in kwargs:
        runtime_env = _container_runtime_env(args)
        if runtime_env is not None:
            kwargs["env"] = runtime_env
    return subprocess.run(args, check=False, **kwargs)


def _check(args: Sequence[str]) -> None:
    # Container management output must not leak into the action's stdout (e.g. the
    # container id printed by `docker run -d`); tool output still streams through.
    result = _run(args, stdout=subprocess.DEVNULL)
    if result.returncode:
        raise SystemExit(result.returncode)


def _safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


_WORKSPACE_LABEL = "com.mongodb.linux-container-actions.workspace"
_CONFIG_FILENAME = "mongo_linux_container_actions.json"
_ACTION_TEMP_DIRNAME = "mongo_linux_action_tmp"
_CONTAINER_ACTION_TEMP_ROOT = pathlib.PurePosixPath("/mongo-tmp")
_ACTION_CONTAINER_ENV = "MONGO_LINUX_ACTION_CONTAINER"
_SHARED_INSTALL_ENV = "MONGO_BAZEL_SHARED_INSTALL_DIR"
_PODMAN_RUNTIME_DIR_PREFIX = "mongo-linux-podman-runtime-"
_PODMAN_STORAGE_DIR_PREFIX = "mongo-linux-podman-storage-"
_PODMAN_TASK_ID_ENV = "MONGO_PODMAN_TASK_ID"
_TRUSTED_CONFIG_ROOT = pathlib.Path("/var/tmp")
_TRUSTED_CONFIG_DIR_PREFIX = "mongo-linux-action-config-"
_MOUNT_PROBE_PREFIX = ".mongo-linux-action-mount-"
_CONTAINER_LAYOUT_VERSION = "v5"


def _is_podman_command(command: Sequence[str]) -> bool:
    return any(pathlib.Path(part).name == "podman" for part in command)


def _podman_task_root(env: Mapping[str, str]) -> pathlib.Path:
    root = pathlib.Path("/tmp")
    task_id = env.get(_PODMAN_TASK_ID_ENV, "")
    if task_id:
        root /= f"mongo-linux-podman-task-{_safe_name(task_id)}"
    return root


def _ensure_owned_directory(path: pathlib.Path, uid: int) -> None:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != uid:
        raise OSError(f"Podman directory is not a directory owned by uid {uid}: {path}")
    path.chmod(0o700)


def _podman_storage_config(runtime_dir: pathlib.Path) -> pathlib.Path:
    uid = os.getuid()
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


def _podman_runtime_dir() -> pathlib.Path:
    """Return a private, task-scoped runtime directory outside the action workspace."""
    uid = os.getuid()
    path = _podman_task_root(os.environ) / f"{_PODMAN_RUNTIME_DIR_PREFIX}{uid}"
    _ensure_owned_directory(path, uid)
    return path


def _trusted_config_dir() -> pathlib.Path:
    """Return persistent host-only state that contained actions cannot mutate."""
    uid = os.getuid()
    # Unlike action and runtime state, trusted configs must survive a reboot. /var/tmp is
    # not mounted into the action container and is conventionally retained across reboots.
    _TRUSTED_CONFIG_ROOT.mkdir(parents=True, exist_ok=True)
    path = _TRUSTED_CONFIG_ROOT / f"{_TRUSTED_CONFIG_DIR_PREFIX}{uid}"
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass

    metadata = path.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != uid:
        raise OSError(f"Trusted action config directory is not owned by this user: {path}")
    path.chmod(0o700)
    return path


def _trusted_config_path(config_path: pathlib.Path) -> pathlib.Path:
    resolved = config_path.resolve()
    digest = hashlib.sha256(str(resolved).encode()).hexdigest()
    return _trusted_config_dir() / f"{digest}.json"


def _publish_trusted_config(config_path: pathlib.Path, config: dict) -> pathlib.Path:
    """Atomically snapshot config outside every path mounted into the action container."""
    destination = _trusted_config_path(config_path)
    content = json.dumps(config, indent=2, sort_keys=True) + "\n"
    try:
        if destination.read_text(encoding="utf-8") == content:
            return destination
    except OSError:
        pass

    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.{uuid.uuid4().hex}")
    descriptor = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        0o600,
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def _read_config(config_path: pathlib.Path) -> dict:
    value = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("container action config must be a JSON object")
    if value.get("container_layout_version") != _CONTAINER_LAYOUT_VERSION:
        raise ValueError(
            "unsupported container action layout; " f"expected {_CONTAINER_LAYOUT_VERSION}"
        )
    missing = [key for key in ("sandbox_base", "shared_install_dir") if key not in value]
    if missing:
        raise ValueError(f"container action config is missing required keys: {', '.join(missing)}")
    return value


def _read_trusted_config(config_path: pathlib.Path) -> dict:
    return _read_config(_trusted_config_path(config_path))


def _container_runtime_env(command: Sequence[str]) -> dict[str, str] | None:
    """Keep rootless Podman's runtime data out of an inherited task TMPDIR."""
    if not _is_podman_command(command):
        return None

    runtime_dir = _podman_runtime_dir()
    storage_config = _podman_storage_config(runtime_dir)
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
    return runtime_env


def _workspace_digest(config: dict, output_base: pathlib.Path) -> str:
    return hashlib.sha256(f"{config['repo_root']}|{output_base}".encode()).hexdigest()[:12]


def _container_name(config: dict, output_base: pathlib.Path) -> str:
    runtime_identity = {
        "version": _CONTAINER_LAYOUT_VERSION,
        "docker_command": config.get("docker_command", "docker"),
        "home": config.get("home", ""),
        "image": config.get("image", ""),
        "layout": config.get("container_layout_version", ""),
        "network": config.get("network", ""),
        "output_base": str(output_base),
        "output_base_generation": config.get("output_base_generation", ""),
        "podman_task_id": config.get("podman_task_id", ""),
        "repo_root": config.get("repo_root", ""),
        "sandbox_base": config.get("sandbox_base", ""),
        "shared_install_dir": config.get("shared_install_dir", ""),
        "state_dir": config.get("state_dir", ""),
        "user": config.get("user", ""),
    }
    runtime_digest = hashlib.sha256(
        json.dumps(runtime_identity, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()[:12]
    suffix = f"_{runtime_digest}"
    configured_name = config.get("container_name")
    if configured_name:
        configured_name = _safe_name(configured_name)
        return f"{configured_name[: 120 - len(suffix)]}{suffix}"
    prefix = config.get("container_prefix", "mongo_linux_action")
    return _safe_name(f"{prefix}{suffix}")[:120]


def _container_running(docker: Sequence[str], name: str) -> bool:
    result = _run(
        [*docker, "inspect", "-f", "{{.State.Running}}", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.returncode == 0 and result.stdout.strip() == "true"


def _container_exists(docker: Sequence[str], name: str) -> bool:
    result = _run(
        [*docker, "inspect", name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def _remove_stale_containers(docker: Sequence[str], workspace: str, keep: str) -> None:
    result = _run(
        [
            *docker,
            "ps",
            "-a",
            "--filter",
            f"label={_WORKSPACE_LABEL}={workspace}",
            "--format",
            "{{.Names}}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode:
        return
    for name in result.stdout.split():
        if name and name != keep:
            _run(
                [*docker, "rm", "-f", name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


def _volume(
    path: pathlib.Path,
    target: pathlib.PurePath | None = None,
    *,
    read_only: bool = False,
) -> list[str]:
    mode = ":ro" if read_only else ""
    return ["-v", f"{path}:{target or path}{mode}"]


def _external_tool_mounts(
    output_base: pathlib.Path,
    mounted_paths: Sequence[pathlib.Path],
) -> list[str]:
    """Mount external repository symlink targets that are outside the output base.

    Bazel keeps a few built-in repositories outside the output base and exposes them through
    absolute symlinks in ``execroot/external``. The action container sees the symlink, but not its
    target, unless the target is mounted explicitly. Workspace and output-base repositories are
    already covered by the normal mounts and must not be mounted a second time.
    """
    mounts: list[str] = []
    mounted_targets: set[pathlib.Path] = set()
    external_roots = [output_base / "external"]
    execroot = output_base / "execroot"
    try:
        external_roots.extend(
            workspace_root / "external"
            for workspace_root in execroot.iterdir()
            if workspace_root.is_dir()
        )
    except OSError:
        pass

    for external_root in external_roots:
        try:
            entries = sorted(external_root.iterdir(), key=lambda path: path.name)
        except OSError:
            continue
        for entry in entries:
            if not entry.is_symlink():
                continue
            try:
                target = entry.resolve(strict=True)
            except OSError:
                continue
            if target in mounted_targets or any(
                target == mounted_path or target.is_relative_to(mounted_path)
                for mounted_path in mounted_paths
            ):
                continue
            mounted_targets.add(target)
            mounts.extend(_volume(target, target, read_only=True))
    return mounts


def _configured_path(config: dict, key: str) -> pathlib.Path:
    configured = config[key]
    path = pathlib.Path(configured)
    if not path.is_absolute():
        raise ValueError(f"container action {key} must be absolute: {path}")
    return path


def _start_container(
    docker: Sequence[str],
    config: dict,
    name: str,
    output_base: pathlib.Path,
    config_path: pathlib.Path | None = None,
) -> None:
    if _container_running(docker, name):
        return

    if _container_exists(docker, name):
        # The deterministic name identifies this exact workspace/output-base/image
        # configuration, so a stopped container is reusable. Starting it is also safe
        # when another action races us: runtimes serialize the transition by name.
        _run(
            [*docker, "start", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if _container_running(docker, name):
            return

        # Remove an unrecoverable stopped container without -f. If another action
        # started it after our check, the removal fails and we adopt the running
        # container instead of disrupting that action.
        result = _run(
            [*docker, "rm", name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if result.returncode:
            if _container_running(docker, name):
                return
            if _container_exists(docker, name):
                raise SystemExit(result.returncode)

    repo_root = pathlib.Path(config["repo_root"])
    config_path = config_path or output_base / _CONFIG_FILENAME
    action_temp_root = output_base / _ACTION_TEMP_DIRNAME
    action_sandbox_root = _configured_path(config, "sandbox_base")
    shared_install_dir = _configured_path(config, "shared_install_dir")
    action_temp_root.mkdir(parents=True, exist_ok=True)
    action_sandbox_root.mkdir(parents=True, exist_ok=True)
    shared_install_dir.mkdir(parents=True, exist_ok=True)

    # Containers with stale identities carry this workspace's label but a different name;
    # reclaim them so they do not accumulate. The label keys on repo root and output base,
    # so other checkouts' containers are never touched.
    workspace = _workspace_digest(config, output_base)
    _remove_stale_containers(docker, workspace, keep=name)

    command = [
        *docker,
        "run",
        "-d",
        "--init",
        "--name",
        name,
        "--label",
        f"{_WORKSPACE_LABEL}={workspace}",
        "-w",
        str(repo_root),
        "-e",
        f"{_ACTION_CONTAINER_ENV}=1",
        *_volume(repo_root, read_only=True),
        *_volume(output_base, read_only=True),
        # processwrapper-sandbox creates one child directory per action below this
        # separate sandbox base. Keep it writable while the Bazel output base,
        # including cache and execroot state, remains read-only.
        *_volume(action_sandbox_root),
        *_volume(shared_install_dir),
        *_volume(config_path, read_only=True),
        *_volume(action_temp_root, _CONTAINER_ACTION_TEMP_ROOT),
        *_external_tool_mounts(
            output_base,
            [repo_root, output_base, action_sandbox_root, shared_install_dir, action_temp_root],
        ),
    ]

    # Rootless Podman on SELinux hosts otherwise relabels or denies the repository and
    # output-base bind mounts. The container is dedicated to this workspace, so disabling
    # SELinux separation for it preserves the same bind-mount semantics as Docker.
    if _is_podman_command(docker):
        command.append("--security-opt=label=disable")
        if os.geteuid() != 0:
            # Rootless Podman normally maps the calling user to container UID 0.
            # Preserve the numeric host identity because the container runs with the
            # same UID:GID and writes directly into host-owned bind mounts.
            command.append("--userns=keep-id")

    network = config.get("network")
    if network:
        command.extend(["--network", network])

    user = config.get("user")
    if user:
        command.extend(["-u", user])

    command.extend(
        [
            config["image"],
            "/bin/bash",
            "-c",
            "trap 'exit 0' TERM INT; while true; do sleep 3600; done",
        ]
    )
    result = _run(command, stdout=subprocess.DEVNULL)
    if result.returncode:
        # A cancelled `docker run -d`/`podman run -d` can create and start the
        # deterministic container before its client reports success. Reuse that exact
        # container instead of failing every subsequent action with a name conflict.
        if _container_running(docker, name):
            return
        if _container_exists(docker, name):
            _run(
                [*docker, "start", name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if _container_running(docker, name):
                return
        raise SystemExit(result.returncode)
    if not _container_running(docker, name):
        print(
            f"ERROR: container runtime reported success but {name} is not running",
            file=sys.stderr,
        )
        raise SystemExit(1)


def _ensure_container(config: dict, config_path: pathlib.Path) -> str:
    docker = shlex.split(config.get("docker_command") or "docker")
    output_base = config_path.parent
    name = _container_name(config, output_base)
    # The action may run below Bazel's linux-sandbox, where the source repository
    # (including the wrapper hook's state directory) is intentionally read-only.
    # The deterministic container name is the cross-process lock: concurrent runtime
    # creates are resolved by _start_container's inspect/adopt path.
    _start_container(docker, config, name, output_base, config_path)
    return name


def _preflight_container(docker: Sequence[str], name: str) -> None:
    """Verify that the selected runtime can execute a process in the container."""
    result = _run(
        [*docker, "exec", name, "/bin/true"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode == 0:
        return

    detail = (result.stderr or result.stdout).strip()
    if len(detail) > 2000:
        detail = detail[:975] + "\n... output truncated ...\n" + detail[-975:]
    detail_suffix = f": {detail}" if detail else ""
    print(
        f"ERROR: container runtime could not execute a process in {name}{detail_suffix}",
        file=sys.stderr,
    )
    raise SystemExit(result.returncode)


def _container_mounts_current(
    docker: Sequence[str],
    name: str,
    output_base: pathlib.Path,
    config: dict,
) -> bool:
    """Return whether the container sees and enforces the current host bind mounts."""
    action_temp_root = output_base / _ACTION_TEMP_DIRNAME
    action_sandbox_root = _configured_path(config, "sandbox_base")
    shared_install_dir = _configured_path(config, "shared_install_dir")
    action_temp_root.mkdir(parents=True, exist_ok=True)
    action_sandbox_root.mkdir(parents=True, exist_ok=True)
    shared_install_dir.mkdir(parents=True, exist_ok=True)
    probe_name = f"{_MOUNT_PROBE_PREFIX}{uuid.uuid4().hex}"
    write_name = f"{probe_name}.container-write"
    output_probe = output_base / probe_name
    action_temp_probe = action_temp_root / probe_name
    action_sandbox_probe = action_sandbox_root / probe_name
    action_temp_write = action_temp_root / write_name
    action_sandbox_write = action_sandbox_root / write_name
    shared_install_probe = shared_install_dir / probe_name
    shared_install_write = shared_install_dir / write_name
    try:
        output_probe.touch(mode=0o600, exist_ok=False)
        action_temp_probe.touch(mode=0o600, exist_ok=False)
        action_sandbox_probe.touch(mode=0o600, exist_ok=False)
        shared_install_probe.touch(mode=0o600, exist_ok=False)
        result = _run(
            [
                *docker,
                "exec",
                name,
                "/bin/bash",
                "-c",
                (
                    'set -e; test -f "$1"; test -f "$2"; test -f "$3"; '
                    'test ! -w "$1"; : > "$4"; : > "$5"; rm -f "$4" "$5"; '
                    'test -f "$6"; : > "$7"; rm -f "$6" "$7"'
                ),
                "mount-probe",
                str(output_probe),
                str(_CONTAINER_ACTION_TEMP_ROOT / probe_name),
                str(action_sandbox_probe),
                str(_CONTAINER_ACTION_TEMP_ROOT / write_name),
                str(action_sandbox_write),
                str(shared_install_probe),
                str(shared_install_write),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0
    finally:
        output_probe.unlink(missing_ok=True)
        action_temp_probe.unlink(missing_ok=True)
        action_sandbox_probe.unlink(missing_ok=True)
        action_temp_write.unlink(missing_ok=True)
        action_sandbox_write.unlink(missing_ok=True)
        shared_install_probe.unlink(missing_ok=True)
        shared_install_write.unlink(missing_ok=True)


def _remove_container(docker: Sequence[str], name: str) -> None:
    """Remove a dedicated action container whose runtime state failed validation."""
    _check([*docker, "rm", "-f", name])


def _ensure_preflight_container(config: dict, config_path: pathlib.Path) -> str:
    """Start, validate, and when necessary recreate an action container."""
    docker = shlex.split(config.get("docker_command") or "docker")
    output_base = config_path.parent
    name = _container_name(config, output_base)
    if _container_running(docker, name):
        if _container_mounts_current(docker, name, output_base, config):
            return name
        print(
            f"WARNING: action container {name} failed its writable-mount probe; recreating it",
            file=sys.stderr,
        )
        _remove_container(docker, name)

    # A stopped container may retain stale bind mounts, and a runtime may report a
    # mount in inspect output without actually installing it. Validate real reads and
    # writes after starting, then recreate once automatically before failing closed.
    for attempt in range(2):
        name = _ensure_container(config, config_path)
        try:
            _preflight_container(docker, name)
        except SystemExit:
            mounts_current = False
        else:
            mounts_current = _container_mounts_current(docker, name, output_base, config)
        if mounts_current:
            return name
        if attempt == 0:
            print(
                f"WARNING: action container {name} failed preflight; recreating it once",
                file=sys.stderr,
            )
            _remove_container(docker, name)

    print(
        f"ERROR: action container {name} does not have usable Bazel bind mounts",
        file=sys.stderr,
    )
    raise SystemExit(1)


def _find_libvoidstar_directory(
    cwd: pathlib.Path,
    output_base: pathlib.Path,
) -> pathlib.Path | None:
    """Find the imported libvoidstar repository at a container-visible path.

    Bazel only places repositories used directly by an action in that action's sandboxed
    ``external`` directory. A tool linked against libvoidstar can need the library at runtime
    without declaring the repository as an input, so also search the normal output-base execroot.
    """
    candidates = [
        cwd / "external" / "libvoidstar",
        output_base / "external" / "libvoidstar",
    ]
    external_roots = [output_base / "external"]
    execroot = output_base / "execroot"
    try:
        external_roots.extend(
            workspace_root / "external"
            for workspace_root in sorted(execroot.iterdir(), key=lambda path: path.name)
            if workspace_root.is_dir()
        )
    except OSError:
        pass

    candidates.extend(external_root / "libvoidstar" for external_root in external_roots)

    # Bzlmod may only expose the canonical repository directory, for example
    # ``_main~setup_mongo_toolchains~libvoidstar``, without creating the usual
    # ``external/libvoidstar`` alias in this output base.
    for external_root in external_roots:
        try:
            candidates.extend(
                entry
                for entry in sorted(external_root.iterdir(), key=lambda path: path.name)
                if entry.name == "libvoidstar" or entry.name.endswith("~libvoidstar")
            )
        except OSError:
            continue

    seen: set[pathlib.Path] = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        try:
            if (candidate / "libvoidstar.so").is_file():
                return candidate
        except OSError:
            continue
    return None


def _exec_env(
    cwd: pathlib.Path,
    temp_dir: pathlib.Path,
    output_base: pathlib.Path,
) -> list[str]:
    # Pass the spawn environment through verbatim (including PATH) so the tool sees the
    # exact environment Bazel computed for the action; this is what remote workers see.
    # Temporary and home directories are the exceptions: each local action gets an
    # isolated, writable directory instead of sharing persistent container state.
    env_args = [
        "-e",
        f"PWD={cwd}",
        "-e",
        f"TMPDIR={temp_dir}",
        "-e",
        f"TMP={temp_dir}",
        "-e",
        f"TEMP={temp_dir}",
        "-e",
        f"HOME={temp_dir / 'home'}",
        "-e",
        f"XDG_CACHE_HOME={temp_dir / 'home' / '.cache'}",
        "-e",
        f"XDG_CONFIG_HOME={temp_dir / 'home' / '.config'}",
        "-e",
        f"XDG_DATA_HOME={temp_dir / 'home' / '.local' / 'share'}",
        "-e",
        f"XDG_STATE_HOME={temp_dir / 'home' / '.local' / 'state'}",
        "-e",
        f"{_ACTION_CONTAINER_ENV}=1",
    ]
    libvoidstar_dir = _find_libvoidstar_directory(cwd, output_base)
    if libvoidstar_dir is not None:
        inherited_library_path = os.environ.get("LD_LIBRARY_PATH")
        library_path = str(libvoidstar_dir)
        if inherited_library_path:
            library_path = f"{library_path}{os.pathsep}{inherited_library_path}"
        env_args.extend(["-e", f"LD_LIBRARY_PATH={library_path}"])
    for key, value in os.environ.items():
        if key in {
            "PWD",
            "TMPDIR",
            "TMP",
            "TEMP",
            "HOME",
            "XDG_CACHE_HOME",
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "CONTAINERS_STORAGE_CONF",
            _ACTION_CONTAINER_ENV,
        }:
            continue
        if libvoidstar_dir is not None and key == "LD_LIBRARY_PATH":
            continue
        if "\x00" in key or "\x00" in value:
            continue
        env_args.extend(["-e", f"{key}={value}"])
    return env_args


# Runs inside the container. Records the shell pid so the host side can cancel this
# action, runs the tool as a child so TERM/INT can be forwarded to it, and cleans up the
# pid file when done.
_CONTAINER_SHIM = """
pidfile="$1"
groupfile="$2"
shift 2
child=""
cancelled=0
forward_signal() {
    cancelled="$2"
    if [[ -s "$groupfile" ]]; then
        kill -"$1" -- "-$(cat "$groupfile")" 2>/dev/null || true
    elif [[ -n "$child" ]]; then
        # setsid makes the action process the process-group leader. This fallback covers
        # the short interval before the action can publish its process group.
        kill -"$1" -- "-$child" 2>/dev/null || true
    fi
}
trap 'forward_signal TERM 143' TERM
trap 'forward_signal INT 130' INT
echo "$$" > "$pidfile"
if [[ "$cancelled" -ne 0 ]]; then
    rm -f "$pidfile"
    exit "$cancelled"
fi
# Publish the action's process group before executing the tool. All of the tool's
# descendants inherit this group, so cancellation cannot leave a grandchild running.
setsid -- /bin/bash -c 'echo "$$" > "$1"; shift; exec "$@"' action-process "$groupfile" "$@" &
child=$!
if [[ "$cancelled" -ne 0 ]]; then
    forward_signal TERM "$cancelled"
fi
wait "$child"
rc=$?
# Bash can return from wait when a trapped signal is handled even though the child is
# still running. Keep waiting so the pid files are not removed while cancellation is
# still in progress.
while kill -0 "$child" 2>/dev/null; do
    wait "$child"
    rc=$?
done
rm -f "$pidfile"
rm -f "$groupfile"
exit "$rc"
"""

_DIAGNOSTICS_SHIM = r"""
echo "Linux container action diagnostics:" >&2
id >&2
for label in cwd tmpdir repo_root output_base; do
    path="$1"
    shift
    printf '  %s=%s\n' "$label" "$path" >&2
    if stat -c '    owner=%u:%g mode=%a type=%F' "$path" >&2; then
        access=""
        test -r "$path" && access="${access}r"
        test -w "$path" && access="${access}w"
        test -x "$path" && access="${access}x"
        printf '    access=%s\n' "${access:-none}" >&2
    fi
done
"""


def _emit_failure_diagnostics(
    docker: Sequence[str],
    name: str,
    cwd: pathlib.Path,
    temp_dir: pathlib.Path,
    repo_root: pathlib.Path,
    output_base: pathlib.Path,
) -> None:
    _run(
        [
            *docker,
            "exec",
            name,
            "/bin/bash",
            "-c",
            _DIAGNOSTICS_SHIM,
            "container-diagnostics",
            str(cwd),
            str(temp_dir),
            str(repo_root),
            str(output_base),
        ],
        stdout=sys.stderr,
        stderr=sys.stderr,
    )


def _cancellation_command(
    docker: Sequence[str],
    name: str,
    pidfile: str,
    groupfile: str,
    container_signal: str,
) -> list[str]:
    quoted_pidfile = shlex.quote(pidfile)
    quoted_groupfile = shlex.quote(groupfile)
    return [
        *docker,
        "exec",
        name,
        "/bin/bash",
        "-c",
        (
            "attempt=0; "
            "while [[ $attempt -lt 200 ]]; do "
            f"if [[ -s {quoted_groupfile} ]]; then "
            f'kill -{container_signal} -- "-$(cat {quoted_groupfile})" 2>/dev/null || true; '
            "exit 0; fi; "
            f'if [[ "{container_signal}" != KILL && -s {quoted_pidfile} ]]; then '
            f'kill -{container_signal} "$(cat {quoted_pidfile})" 2>/dev/null || true; '
            "exit 0; fi; "
            "attempt=$((attempt + 1)); sleep 0.01; "
            f"done; if [[ -s {quoted_pidfile} ]]; then "
            f'kill -{container_signal} "$(cat {quoted_pidfile})" 2>/dev/null || true; '
            "fi; exit 0"
        ),
    ]


def _start_cancellation_process(
    docker: Sequence[str],
    name: str,
    pidfile: str,
    groupfile: str,
    container_signal: str,
) -> subprocess.Popen:
    kwargs = {
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
    }
    runtime_env = _container_runtime_env(docker)
    if runtime_env is not None:
        kwargs["env"] = runtime_env
    return subprocess.Popen(
        _cancellation_command(docker, name, pidfile, groupfile, container_signal),
        **kwargs,
    )


def _run_in_container(
    config: dict,
    config_path: pathlib.Path,
    real_tool: pathlib.Path,
    tool_args: Sequence[str],
) -> int:
    docker = shlex.split(config.get("docker_command") or "docker")
    output_base = config_path.parent
    cwd = pathlib.Path.cwd()
    name = _container_name(config, output_base)
    pidfile = f"/tmp/mongo-action-{uuid.uuid4().hex}.pid"
    groupfile = f"/tmp/mongo-action-{uuid.uuid4().hex}.pgid"
    cancelled: list[int] = []
    cancellation_processes: list[subprocess.Popen] = []
    process: subprocess.Popen | None = None

    def _kill_in_container(container_signal: str) -> None:
        cancellation_processes.append(
            _start_cancellation_process(docker, name, pidfile, groupfile, container_signal)
        )

    def _forward_signal(signum, frame):
        # Dynamic scheduling cancels the losing branch with SIGTERM; forward it into the
        # container so the tool does not keep running as an orphan. Escalate to SIGKILL
        # if a second signal arrives, then stop waiting on the docker exec client.
        cancelled.append(signum)
        if len(cancelled) == 1:
            _kill_in_container("TERM")
        else:
            _kill_in_container("KILL")
            if process is not None:
                process.kill()

    previous_handlers = {
        signal.SIGTERM: signal.signal(signal.SIGTERM, _forward_signal),
        signal.SIGINT: signal.signal(signal.SIGINT, _forward_signal),
    }
    temp_dir: pathlib.Path | None = None
    try:
        action_temp_root = output_base / _ACTION_TEMP_DIRNAME
        action_temp_root.mkdir(parents=True, exist_ok=True)
        temp_dir = pathlib.Path(tempfile.mkdtemp(prefix="action-", dir=action_temp_root))
        (temp_dir / "home").mkdir(mode=0o700)
        container_temp_dir = _CONTAINER_ACTION_TEMP_ROOT / temp_dir.name
        if cancelled:
            return 128 + int(cancelled[0])
        popen_kwargs = {}
        runtime_env = _container_runtime_env(docker)
        if runtime_env is not None:
            popen_kwargs["env"] = runtime_env
        action_command = [
            *docker,
            "exec",
            "-i",
            "-w",
            str(cwd),
            *_exec_env(cwd, container_temp_dir, output_base),
            "-e",
            f"{_SHARED_INSTALL_ENV}={config['shared_install_dir']}",
            name,
            "/bin/bash",
            "-c",
            _CONTAINER_SHIM,
            "container-shim",
            pidfile,
            groupfile,
            str(real_tool),
            *tool_args,
        ]

        def _run_action() -> int:
            nonlocal process
            process = subprocess.Popen(action_command, **popen_kwargs)

            # Handled signals do not interrupt wait() (PEP 475); the handler forwards the
            # signal into the container, the tool exits, and the docker exec client follows.
            process.wait()
            return process.returncode

        # The preflight hook starts and validates the persistent container. Try the action
        # directly on the common path; a container can still be stopped or removed while
        # Bazel is running, so recover lazily only after exec fails.
        returncode = _run_action()
        if returncode and not cancelled and not _container_running(docker, name):
            name = _ensure_container(config, config_path)
            if not cancelled:
                returncode = _run_action()

        if returncode and not cancelled:
            _emit_failure_diagnostics(
                docker,
                name,
                cwd,
                pathlib.Path(container_temp_dir),
                pathlib.Path(config["repo_root"]),
                output_base,
            )
        if cancelled and returncode == 0:
            returncode = 128 + int(cancelled[0])
        return returncode
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        for cancellation_process in cancellation_processes:
            try:
                cancellation_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cancellation_process.kill()
                cancellation_process.wait()
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)


def main(argv: Sequence[str]) -> int:
    ensure_only = len(argv) == 3 and argv[1] == "--ensure-container"
    if not ensure_only and len(argv) < 3:
        print(
            "usage: linux_container_action_wrapper.py "
            "[--ensure-container] CONFIG_JSON [REAL_TOOL [ARGS...]]",
            file=sys.stderr,
        )
        return 2

    config_path = pathlib.Path(argv[2] if ensure_only else argv[1])

    source = config_path
    try:
        if ensure_only:
            config = _read_config(config_path)
            _publish_trusted_config(config_path, config)
        else:
            source = _trusted_config_path(config_path)
            config = _read_config(source)
    except (OSError, ValueError) as exc:
        print(
            f"ERROR: could not read trusted container action config {source} ({exc}); "
            "refusing to run the build tool natively",
            file=sys.stderr,
        )
        return 2

    # Bazel's persistent-container runner may be launched by a long-lived Bazel server,
    # whose environment predates the current Evergreen task. The trusted config is
    # published by the wrapper hook after task-specific Podman setup, so restore the
    # task identity before any Podman command is constructed.
    if "podman_task_id" in config:
        task_id = config["podman_task_id"]
        if task_id:
            os.environ[_PODMAN_TASK_ID_ENV] = task_id
        else:
            os.environ.pop(_PODMAN_TASK_ID_ENV, None)

    if ensure_only:
        _ensure_preflight_container(config, config_path)
        return 0

    real_tool = pathlib.Path(argv[2])
    tool_args = argv[3:]
    return _run_in_container(config, config_path, real_tool, tool_args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
