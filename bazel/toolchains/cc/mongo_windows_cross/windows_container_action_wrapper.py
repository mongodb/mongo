#!/usr/bin/env python3
"""Runs Windows-host cross build actions inside a persistent Linux action container.

Invoked by Bazel's persistent-container spawn strategy on Windows hosts with the same
contract as bazel/toolchains/cc/mongo_linux/linux_container_action_wrapper.py:

    windows_container_action_wrapper.py CONFIG_JSON REAL_TOOL [ARGS...]
    windows_container_action_wrapper.py --ensure-container CONFIG_JSON

CONFIG_JSON is written to the Bazel output base by bazel/wrapper_hook/hermetic_container_integration.py
before the build starts. Bazel prepares the action's declared-input sandbox on the Windows host
and runs this script with the sandbox execroot as its working directory; the script executes
REAL_TOOL at the same location inside the container.

The container is started from the pinned image the Windows cross configuration advertises for
remote execution, so a link that runs here produces the same result as one that runs on RBE.

Windows-specific behaviour, compared with the Linux runner:

* The Linux Docker Engine on the Windows CI images runs inside a WSL2 distribution, where the
  Windows drives appear as ``/mnt/<drive>``. Every host path is translated to that layout and the
  drives are mounted at the same ``/mnt/<drive>`` paths inside the container, so the NTFS symlinks
  Bazel plants in the sandbox resolve to the same targets inside the container.
* That engine is reached over TCP and does not deliver ``docker exec`` output or exit statuses
  reliably, so actions run detached and report through files in the container that are copied
  back with ``docker cp``.
* Windows has no uid mapping into the container; actions run as the image's default user.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Iterator, Mapping, Sequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4]))
from bazel.hermetic_container import container_paths
from bazel.toolchains.cc import container_action as shared
from bazel.toolchains.cc import container_engine as engine

_CONTAINER_LAYOUT_VERSION = "windows-v3"
_WORKSPACE_LABEL = "com.mongodb.windows-container-actions.workspace"
_ACTION_CONTAINER_ENV = "MONGO_LINUX_ACTION_CONTAINER"
_DEFAULT_DRIVE_MOUNT_PREFIX = "/mnt"
_DEFAULT_DOCKER_HOST = "tcp://127.0.0.1:2375"
_WSL_DOCKER_HOST_MODES = frozenset({"wsl", "wsl-local"})
_START_LOCK_FILENAME = "windows-container-actions.lock"
_RUNNER_SCRATCH_DIRNAME = "windows-container-runner"
_MOUNT_PROBE_PREFIX = ".mongo-windows-action-mount-"
_PREFLIGHT_COMMAND_TIMEOUT_SECONDS = 180.0
_DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS = 60.0
_ENV_FILE_UNSUPPORTED_MARKER = "unknown flag: --env-file"
_DRIVE_PATH_RE = re.compile(r"^[A-Za-z]:[\\/]")

# Environment variables that describe the Windows host or this runner's own Docker routing.
# They are meaningless inside the Linux container and must not leak into the action, which
# otherwise sees the environment Bazel computed for it (also what the RBE workers see).
_HOST_ONLY_ENVIRONMENT = frozenset(
    {
        "ALLUSERSPROFILE",
        "APPDATA",
        "COMMONPROGRAMFILES",
        "COMMONPROGRAMFILES(X86)",
        "COMMONPROGRAMW6432",
        "COMPUTERNAME",
        "COMSPEC",
        "DOCKER_API_VERSION",
        "DOCKER_CERT_PATH",
        "DOCKER_CONFIG",
        "DOCKER_CONTEXT",
        "DOCKER_HOST",
        "DOCKER_TLS_VERIFY",
        "DRIVERDATA",
        "HOME",
        "HOMEDRIVE",
        "HOMEPATH",
        "LOCALAPPDATA",
        "LOGONSERVER",
        "NUMBER_OF_PROCESSORS",
        "OS",
        "PATHEXT",
        "PROCESSOR_ARCHITECTURE",
        "PROCESSOR_IDENTIFIER",
        "PROCESSOR_LEVEL",
        "PROCESSOR_REVISION",
        "PROGRAMDATA",
        "PROGRAMFILES",
        "PROGRAMFILES(X86)",
        "PROGRAMW6432",
        "PSMODULEPATH",
        "PUBLIC",
        "PWD",
        "SESSIONNAME",
        "SYSTEMDRIVE",
        "SYSTEMROOT",
        "TEMP",
        "TMP",
        "TMPDIR",
        "USERDOMAIN",
        "USERDOMAIN_ROAMINGPROFILE",
        "USERNAME",
        "USERPROFILE",
        "WINDIR",
        shared.ACTION_OUTPUT_LOG_ENV,
        engine.DOCKER_HOST_MODE_ENV,
        engine.WSL_DISTRIBUTION_ENV,
        engine.WSL_DOCKER_SOCKET_ENV,
    }
)

# The shared supervisor owns private temporary/home directories so cleanup survives tool
# cancellation. Replace the shim with the tool rather than adding another process to reap.
_ACTION_SHIM = (pathlib.Path(__file__).parent / "scripts" / "action.sh").read_text(encoding="utf-8")

# Read scripts on the host and transmit them through the shared runner: they need not
# be mounted inside the container or have an NTFS executable bit.
_MOUNT_PROBE_SCRIPT = (pathlib.Path(__file__).parent / "scripts" / "mount_probe.sh").read_text(
    encoding="utf-8"
)

_DIAGNOSTICS_SCRIPT = (pathlib.Path(__file__).parent / "scripts" / "diagnostics.sh").read_text(
    encoding="utf-8"
)


def _read_config(config_path: pathlib.Path) -> dict:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(config, dict):
        raise ValueError("container action config must be a JSON object")
    for key in ("image", "docker_command", "repo_root", "sandbox_base"):
        if not config.get(key):
            raise ValueError(f"container action config is missing {key!r}")
    return config


def _apply_runner_environment(config: Mapping[str, object]) -> None:
    """Restores the host settings the Docker CLI needs.

    Bazel starts the runner with the action's environment, which has no Windows system
    variables and a PATH made of the toolchain's relative directories. The wrapper hook
    records what the CLI needs when it writes the config.
    """
    runner_environment = config.get("runner_environment") or {}
    if isinstance(runner_environment, Mapping):
        for key, value in runner_environment.items():
            if isinstance(key, str) and isinstance(value, str):
                os.environ[key] = value
    os.environ[engine.DOCKER_HOST_MODE_ENV] = str(config.get("docker_host_mode") or "")
    if config.get("docker_host_mode") == engine.WSL_DOCKER_HOST_MODE:
        os.environ.setdefault("DOCKER_HOST", _DEFAULT_DOCKER_HOST)
    if _uses_wsl_engine(config):
        # DOCKER_CONTEXT takes precedence over DOCKER_HOST in the Docker CLI.
        os.environ.pop("DOCKER_CONTEXT", None)


def _uses_wsl_engine(config: Mapping[str, object]) -> bool:
    return str(config.get("docker_host_mode") or "").lower() in _WSL_DOCKER_HOST_MODES


def _docker_command(config: Mapping[str, object]) -> list[str]:
    docker = shlex.split(str(config["docker_command"]))
    if docker and pathlib.PureWindowsPath(docker[0]).name.lower() == "wsl.exe":
        # A Linux CLI opened through WSL cannot read Windows paths passed to --env-file
        # or docker cp. Keep this boundary local to the runner; shared transport still
        # receives the host paths it uses for files and cleanup.
        return [
            sys.executable,
            str(pathlib.Path(__file__).resolve()),
            "--docker-cli",
            json.dumps(docker),
            _drive_mount_prefix(config),
        ]
    return docker


def _wsl_client_file_args(
    args: Sequence[str], prefix: str = _DEFAULT_DRIVE_MOUNT_PREFIX
) -> list[str]:
    """Translate CLI-owned files without rewriting container paths or action arguments."""
    args = list(args)
    if args and args[0] == "cp":
        for index in range(1, len(args)):
            if _DRIVE_PATH_RE.match(args[index]):
                args[index] = _container_path(args[index], prefix).as_posix()
    elif args and args[0] == "exec":
        index = 1
        value_options = {
            "-w",
            "--workdir",
            "-u",
            "--user",
            "-e",
            "--env",
            "--env-file",
            "--detach-keys",
        }
        while index < len(args) and args[index].startswith("-"):
            option = args[index]
            if option == "--env-file" and index + 1 < len(args):
                index += 1
                if _DRIVE_PATH_RE.match(args[index]):
                    args[index] = _container_path(args[index], prefix).as_posix()
            elif option in value_options:
                index += 1
            index += 1
    return args


def _drive_mount_prefix(config: Mapping[str, object]) -> str:
    return container_paths.drive_mount_prefix(
        str(config.get("drive_mount_prefix") or _DEFAULT_DRIVE_MOUNT_PREFIX)
    )


def _container_path(
    path: pathlib.PurePath | str, prefix: str = _DEFAULT_DRIVE_MOUNT_PREFIX
) -> pathlib.PurePosixPath:
    """Translate a drive-qualified host path using the shared WSL path policy."""
    if not _DRIVE_PATH_RE.match(os.fspath(path)):
        raise ValueError(f"expected an absolute Windows drive path, got {path!r}")
    return pathlib.PurePosixPath(container_paths.wsl_volume_source(path, prefix))


def _drive_letters(paths: Sequence[object]) -> list[str]:
    drives: set[str] = set()
    for path in paths:
        if not path:
            continue
        value = os.fspath(path) if isinstance(path, (str, os.PathLike)) else str(path)
        if _DRIVE_PATH_RE.match(value):
            drives.add(value[0].lower())
    return sorted(drives)


def _mount_paths(config: Mapping[str, object], output_base: pathlib.Path) -> list[str]:
    paths: list[str] = [str(config["repo_root"]), str(output_base), str(config["sandbox_base"])]
    if config.get("shared_install_dir"):
        paths.append(str(config["shared_install_dir"]))
    extra = config.get("mount_paths") or []
    if isinstance(extra, list):
        paths.extend(str(path) for path in extra if path)
    return paths


def _container_mounts(config: Mapping[str, object], output_base: pathlib.Path) -> list[str]:
    """Returns the ``-v`` options that expose the host drives inside the container.

    Whole drives are mounted read-only at the daemon's own ``/mnt/<drive>`` paths, so absolute
    NTFS symlink targets (which drvfs reports as ``/mnt/<drive>/...``) resolve inside the
    container exactly as they do in the WSL distribution. Only the Bazel sandbox base, where
    actions write their outputs, and the shared install directory (compile-command replay
    outputs) are mounted writable.
    """
    prefix = _drive_mount_prefix(config)
    mounts: list[str] = []
    for drive in _drive_letters(_mount_paths(config, output_base)):
        source = f"{prefix}/{drive}"
        native_source = f"{drive.upper()}:\\"
        host_source = source if _uses_wsl_engine(config) else native_source
        mounts.extend(["-v", f"{host_source}:{source}:ro"])
    for writable in _writable_paths(config):
        container_path = _container_path(writable, prefix)
        host_source = str(container_path) if _uses_wsl_engine(config) else str(writable)
        mounts.extend(["-v", f"{host_source}:{container_path}"])
    return mounts


def _writable_paths(config: Mapping[str, object]) -> list[str]:
    writable = [str(config["sandbox_base"])]
    if config.get("shared_install_dir"):
        writable.append(str(config["shared_install_dir"]))
    return writable


def _workspace_digest(config: Mapping[str, object], output_base: pathlib.Path) -> str:
    return hashlib.sha256(f"{config['repo_root']}|{output_base}".encode()).hexdigest()[:12]


def _container_name(config: Mapping[str, object], output_base: pathlib.Path) -> str:
    runtime_identity = {
        "version": _CONTAINER_LAYOUT_VERSION,
        "docker_command": config.get("docker_command", "docker"),
        "docker_host": os.environ.get("DOCKER_HOST", ""),
        "docker_context": os.environ.get("DOCKER_CONTEXT", ""),
        "docker_host_mode": config.get("docker_host_mode", ""),
        "drive_mount_prefix": config.get("drive_mount_prefix", ""),
        "image": config.get("image", ""),
        "layout": config.get("container_layout_version", ""),
        "mount_paths": sorted(str(path) for path in (config.get("mount_paths") or [])),
        "network": config.get("network", ""),
        "output_base": str(output_base),
        "output_base_generation": config.get("output_base_generation", ""),
        "repo_root": config.get("repo_root", ""),
        "sandbox_base": config.get("sandbox_base", ""),
        "shared_install_dir": config.get("shared_install_dir", ""),
    }
    runtime_digest = hashlib.sha256(
        json.dumps(runtime_identity, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()[:12]
    suffix = f"_{runtime_digest}"
    configured_name = config.get("container_name")
    if configured_name:
        configured_name = shared.safe_name(str(configured_name))
        return f"{configured_name[: 120 - len(suffix)]}{suffix}"
    prefix = str(config.get("container_prefix") or "mongo_windows_cross_action")
    return shared.safe_name(f"{prefix}{suffix}")[:120]


def _scratch_root(config: Mapping[str, object]) -> pathlib.Path:
    """Host directory for the short-lived copies of container-side status files."""
    return pathlib.Path(str(config["sandbox_base"])) / _RUNNER_SCRATCH_DIRNAME


@contextlib.contextmanager
def _start_lock(config: Mapping[str, object]) -> Iterator[None]:
    lock_path = _scratch_root(config) / _START_LOCK_FILENAME
    with shared.exclusive_file_lock(lock_path, required=True):
        yield


def _remove_stale_containers(docker: Sequence[str], workspace: str, keep: str) -> None:
    result = shared.run(
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
    )
    if result.returncode:
        return
    for name in (result.stdout or "").split():
        if name and name != keep:
            shared.run(
                [*docker, "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )


def _mount_host_drives(
    docker: Sequence[str], config: Mapping[str, object], output_base: pathlib.Path
) -> None:
    """Makes sure the WSL distribution behind the Docker Engine has the host drives mounted.

    Docker silently bind-mounts an empty directory when a source is missing in the daemon's
    filesystem, which would make every action input invisible. The mount is attempted through
    a privileged helper container because ``wsl.exe`` cannot reach a distribution that is
    registered to another Windows user.
    """
    if not _uses_wsl_engine(config) or not os.environ.get("DOCKER_HOST", "").startswith("tcp://"):
        return
    drives = _drive_letters(_mount_paths(config, output_base))
    if not drives:
        return
    prefix = _drive_mount_prefix(config)
    print(
        "INFO: ensuring drives "
        + ", ".join(f"{drive.upper()}:" for drive in drives)
        + f" are mounted under {prefix} inside the WSL Docker Engine's distribution",
        file=sys.stderr,
    )
    mounted, detail = shared.mount_windows_drives_in_docker_vm(
        docker, str(config["image"]), drives, prefix
    )
    if mounted:
        print(f"INFO: WSL Docker drive mounts:\n{detail}", file=sys.stderr)
    else:
        print(
            f"WARNING: could not mount host drives inside the WSL Docker Engine's distribution: {detail}",
            file=sys.stderr,
        )


def _start_container(
    docker: Sequence[str],
    config: Mapping[str, object],
    name: str,
    output_base: pathlib.Path,
) -> None:
    if shared.container_running(docker, name):
        return

    if shared.container_exists(docker, name):
        # The deterministic name identifies this exact workspace/output-base/image
        # configuration, so a stopped container is reusable.
        shared.run([*docker, "start", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if shared.container_running(docker, name):
            return
        result = shared.run(
            [*docker, "rm", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        if result.returncode:
            if shared.container_running(docker, name):
                return
            if shared.container_exists(docker, name):
                raise SystemExit(result.returncode)

    for writable in _writable_paths(config):
        pathlib.Path(writable).mkdir(parents=True, exist_ok=True)
    _scratch_root(config).mkdir(parents=True, exist_ok=True)
    _mount_host_drives(docker, config, output_base)

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
        # Every exec sets its own working directory. Do not point the container at a
        # drive path here: Docker would try to create it, which fails on the read-only
        # drive mounts when the WSL distribution has not mounted the drive yet, hiding
        # the far more useful mount-probe diagnostics.
        "-w",
        "/",
        "-e",
        f"{_ACTION_CONTAINER_ENV}=1",
        *_container_mounts(config, output_base),
    ]
    network = config.get("network")
    if network:
        command.extend(["--network", str(network)])
    command.extend(
        [
            str(config["image"]),
            "/bin/bash",
            "-c",
            "trap 'exit 0' TERM INT; while true; do sleep 3600; done",
        ]
    )
    result = shared.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # A cancelled ``docker run -d`` can create and start the container before its client
        # reports success; adopt that container rather than failing on a name conflict.
        if shared.container_running(docker, name):
            return
        if shared.container_exists(docker, name):
            shared.run(
                [*docker, "start", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            if shared.container_running(docker, name):
                return
        detail = ((result.stderr or "") + (result.stdout or "")).strip()
        print(
            f"ERROR: could not start action container {name}: {detail or '<no detail>'}",
            file=sys.stderr,
        )
        raise SystemExit(result.returncode)
    if not shared.container_running(docker, name):
        print(
            f"ERROR: container runtime reported success but {name} is not running", file=sys.stderr
        )
        raise SystemExit(1)


def _ensure_container(
    docker: Sequence[str], config: Mapping[str, object], output_base: pathlib.Path
) -> str:
    name = _container_name(config, output_base)
    with _start_lock(config):
        if not engine.start_container_with_recovery(
            docker, lambda: _start_container(docker, config, name, output_base)
        ):
            raise SystemExit(125)
    return name


def _remove_container(docker: Sequence[str], name: str) -> None:
    shared.run([*docker, "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _verify_engine(docker: Sequence[str]) -> None:
    os_type = shared.docker_info_os_type(docker)
    if os_type != "linux":
        detail = f"reported OS type {os_type!r}" if os_type else "did not answer `docker info`"
        print(
            f"ERROR: the Docker Engine at {os.environ.get('DOCKER_HOST') or '<default>'} {detail}; "
            "Windows cross actions need the Linux engine in the WSL distribution",
            file=sys.stderr,
        )
        raise SystemExit(1)


def _mount_problems(
    docker: Sequence[str],
    config: Mapping[str, object],
    name: str,
    output_base: pathlib.Path,
) -> str:
    """Returns the probe output when the container cannot see or write the Bazel paths, else ''."""
    prefix = _drive_mount_prefix(config)
    repo_root = pathlib.Path(str(config["repo_root"]))
    sandbox_base = pathlib.Path(str(config["sandbox_base"]))
    probe_name = f"{_MOUNT_PROBE_PREFIX}{uuid.uuid4().hex}"
    output_probe = output_base / probe_name
    repo_probe_dir = repo_root / ".tmp" / "windows_container_actions"
    repo_probe = repo_probe_dir / probe_name
    sandbox_probe = sandbox_base / probe_name
    link_probe = sandbox_base / f"{probe_name}.link"
    created: list[pathlib.Path] = []
    link_error = ""
    try:
        output_base.mkdir(parents=True, exist_ok=True)
        repo_probe_dir.mkdir(parents=True, exist_ok=True)
        sandbox_base.mkdir(parents=True, exist_ok=True)
        for probe in (output_probe, repo_probe, sandbox_probe):
            probe.write_text("probe\n", encoding="utf-8")
            created.append(probe)
        try:
            # Bazel plants NTFS symlinks to the real inputs in every sandbox; the container
            # must be able to follow them through the drvfs mounts.
            os.symlink(output_probe, link_probe)
            created.append(link_probe)
        except OSError as exc:
            link_error = f"could not create an NTFS symlink probe on the host ({exc})"
            link_probe = None
        expected_target = _container_path(output_probe, prefix).as_posix()
        exit_code, output, _ = shared.run_container_command(
            docker,
            name,
            _container_path(repo_root, prefix),
            [],
            [
                "/bin/sh",
                "-c",
                _MOUNT_PROBE_SCRIPT,
                "mount-probe",
                _container_path(output_probe, prefix).as_posix(),
                _container_path(repo_probe, prefix).as_posix(),
                _container_path(sandbox_probe, prefix).as_posix(),
                _container_path(link_probe, prefix).as_posix() if link_probe is not None else "",
                expected_target,
            ],
            _scratch_root(config) / shared.ACTION_OUTPUT_LOG_DIRNAME,
            timeout=_PREFLIGHT_COMMAND_TIMEOUT_SECONDS,
        )
    finally:
        for probe in created:
            try:
                probe.unlink()
            except OSError:
                pass
    output = output.rstrip()
    if link_error:
        return f"{output}\nERROR: {link_error}".strip()
    if exit_code == 0:
        # Keep the successful probe visible in build logs: it is the only place that records
        # whether NTFS symlinks resolved through the drive mounts on this host.
        for line in output.splitlines():
            print(f"INFO: mount probe: {line}", file=sys.stderr)
        return ""
    return output or f"mount probe exited with status {exit_code} and produced no output"


def _container_summary(docker: Sequence[str], name: str) -> str:
    result = shared.run(
        [
            *docker,
            "inspect",
            "-f",
            "{{.State.Status}} image={{.Config.Image}}{{range .Mounts}} {{.Source}}->{{.Destination}}{{if .RW}}(rw){{else}}(ro){{end}}{{end}}",
            name,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return ((result.stdout or "") + (result.stderr or "")).strip()


def _ensure_preflight_container(config: Mapping[str, object], config_path: pathlib.Path) -> str:
    """Starts the container and verifies its bind mounts, recreating it once if they are wrong."""
    docker = _docker_command(config)
    output_base = config_path.parent
    _verify_engine(docker)
    problems = ""
    for attempt in range(2):
        name = _ensure_container(docker, config, output_base)
        problems = _mount_problems(docker, config, name, output_base)
        if not problems:
            print(
                f"INFO: action container {name} sees the Bazel workspace, output base, and sandbox",
                file=sys.stderr,
            )
            return name
        if attempt == 0:
            print(
                f"WARNING: action container {name} failed its mount probe; recreating it once:\n{problems}",
                file=sys.stderr,
            )
            _remove_container(docker, name)

    print(
        f"ERROR: action container {name} does not expose the Bazel paths from the Windows host:\n"
        f"{problems}\n"
        f"container: {_container_summary(docker, name)}\n"
        "The Docker Engine's WSL distribution must have the Windows drives mounted under "
        f"{_drive_mount_prefix(config)} (drvfs) and be able to follow NTFS symlinks through them.",
        file=sys.stderr,
    )
    raise SystemExit(1)


def _container_tool(real_tool: str, prefix: str) -> str:
    """Normalizes the action's argv[0] for the container.

    Bazel may spell the executable with backslashes on Windows. Execroot-relative tools stay
    relative (they resolve against the sandbox execroot, the container's working directory);
    absolute Windows paths are translated to the drive mounts.
    """
    if _DRIVE_PATH_RE.match(real_tool):
        return _container_path(real_tool, prefix).as_posix()
    return real_tool.replace("\\", "/")


def _tool_command(tool: str) -> list[str]:
    """Returns how the container should start the action's executable.

    The Windows-host toolchain names its tools by their ``.cmd`` wrappers. Their first line is
    a POSIX command that execs the real Linux tool, which is how RBE workers run them; start
    them through ``sh`` explicitly so the result does not depend on the execute bit drvfs
    reports for the file.
    """
    if tool.lower().endswith((".cmd", ".bat")):
        return ["/bin/sh", tool]
    return [tool]


def _action_environment(container_cwd: pathlib.PurePosixPath) -> dict[str, str]:
    environment: dict[str, str] = {}
    for key, value in os.environ.items():
        if key.upper() in _HOST_ONLY_ENVIRONMENT or not key or "=" in key:
            continue
        if "\x00" in key or "\x00" in value or "\n" in value or "\r" in value:
            continue
        environment[key] = value
    environment["PWD"] = container_cwd.as_posix()
    environment[_ACTION_CONTAINER_ENV] = "1"
    environment[shared.ACTION_PRIVATE_SCRATCH_ENV] = "1"
    return environment


def _write_env_file(environment: Mapping[str, str], scratch_root: pathlib.Path) -> pathlib.Path:
    """Writes the action environment for ``docker exec --env-file``.

    A link action's environment (LIB, INCLUDE, PATH, ...) can push a ``-e`` per variable past
    the Windows command-line length limit; the CLI reads the file locally.
    """
    scratch_root.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=scratch_root, prefix="action-env-", suffix=".env", text=True
    )
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as env_file:
        for key in sorted(environment):
            env_file.write(f"{key}={environment[key]}\n")
    return pathlib.Path(temporary_name)


def _env_args(environment: Mapping[str, str]) -> list[str]:
    env_args: list[str] = []
    for key in sorted(environment):
        env_args.extend(["-e", f"{key}={environment[key]}"])
    return env_args


def _emit_failure_diagnostics(
    docker: Sequence[str],
    config: Mapping[str, object],
    name: str,
    container_cwd: pathlib.PurePosixPath,
    tool: str,
    launch: Sequence[str],
) -> None:
    print("Windows container action diagnostics:", file=sys.stderr)
    print(f"  docker exec command: {shlex.join(list(launch))}", file=sys.stderr)
    print(f"  container: {_container_summary(docker, name)}", file=sys.stderr)
    _, output, _ = shared.run_container_command(
        docker,
        name,
        container_cwd,
        [],
        ["/bin/sh", "-c", _DIAGNOSTICS_SCRIPT, "diagnostics", container_cwd.as_posix(), tool],
        _scratch_root(config) / shared.ACTION_OUTPUT_LOG_DIRNAME,
        timeout=_DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS,
    )
    for line in output.rstrip().splitlines():
        print(f"  {line}", file=sys.stderr)


def _run_in_container(
    config: Mapping[str, object],
    config_path: pathlib.Path,
    real_tool: str,
    tool_args: Sequence[str],
) -> int:
    docker = _docker_command(config)
    output_base = config_path.parent
    prefix = _drive_mount_prefix(config)
    cwd = pathlib.Path.cwd()
    try:
        container_cwd = _container_path(cwd, prefix)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 125

    name = _container_name(config, output_base)
    if not shared.container_running(docker, name):
        # The preflight normally leaves the container running; recover if it was stopped or
        # removed while Bazel was running.
        name = _ensure_container(docker, config, output_base)

    tool = _container_tool(real_tool, prefix)
    scratch_root = _scratch_root(config)
    environment = _action_environment(container_cwd)
    command = [
        "/bin/sh",
        "-c",
        _ACTION_SHIM,
        "windows-action-shim",
        *_tool_command(tool),
        *tool_args,
    ]
    env_file = _write_env_file(environment, scratch_root)
    try:
        exit_code, output, launch = shared.run_container_command(
            docker,
            name,
            container_cwd,
            ["--env-file", str(env_file)],
            command,
            scratch_root / shared.ACTION_OUTPUT_LOG_DIRNAME,
        )
        if exit_code == 125 and _ENV_FILE_UNSUPPORTED_MARKER in output:
            exit_code, output, launch = shared.run_container_command(
                docker,
                name,
                container_cwd,
                _env_args(environment),
                command,
                scratch_root / shared.ACTION_OUTPUT_LOG_DIRNAME,
            )
    finally:
        env_file.unlink(missing_ok=True)

    if output:
        sys.stderr.write(output if output.endswith("\n") else output + "\n")
        sys.stderr.flush()
    if exit_code in shared.CONTAINER_EXEC_FAILURE_CODES:
        _emit_failure_diagnostics(docker, config, name, container_cwd, tool, launch)
    return exit_code


def _resolve_docker_command(config: Mapping[str, object]) -> list[str] | None:
    """Returns the Docker CLI command with an executable that exists on this host.

    The action environment's PATH only lists toolchain directories, so a bare ``docker`` is
    looked up on the host PATH the wrapper hook recorded in the config.
    """
    argv = shlex.split(str(config["docker_command"]))
    if not argv:
        return None
    executable = pathlib.Path(argv[0])
    if executable.is_absolute() or len(executable.parts) > 1:
        return argv if executable.exists() else None
    for search_path in (str(config.get("host_path") or ""), None):
        found = shutil.which(argv[0], path=search_path or None)
        if found:
            return [found, *argv[1:]]
    return None


def _select_engine(config: Mapping[str, object], docker: Sequence[str]) -> dict:
    """Reuse the shared WSL/Desktop selection policy and retain its effective route."""
    mode = str(config.get("docker_host_mode") or "")
    os.environ[engine.DOCKER_HOST_MODE_ENV] = mode
    selected = engine.select_linux_docker_command(docker)
    if selected is None:
        raise SystemExit(125)
    return {
        **config,
        "docker_command": shlex.join(selected),
        "docker_host_mode": os.environ.get(engine.DOCKER_HOST_MODE_ENV, mode),
        "engine_selected": True,
        "runner_environment": {
            **{
                key: value
                for key, value in (config.get("runner_environment") or {}).items()
                if key not in {"DOCKER_HOST", "DOCKER_CONTEXT"}
            },
            **{
                key: os.environ[key]
                for key in ("DOCKER_HOST", "DOCKER_CONTEXT")
                if key in os.environ
            },
        },
    }


def prepare_runner_config(config: Mapping[str, object]) -> dict:
    """Resolve host CLI settings and select Linux before pulling or starting the image."""
    _apply_runner_environment(config)
    docker = _resolve_docker_command(config)
    if docker is None:
        print(f"ERROR: Docker CLI {config['docker_command']!r} was not found", file=sys.stderr)
        raise SystemExit(125)
    if config.get("engine_selected"):
        # Preflight and every action must use the engine the host selected before pulling.
        # In particular, the TCP fallback may serve a distro invisible to this user.
        return {**config, "docker_command": shlex.join(docker)}
    return _select_engine(config, docker)


def main(argv: Sequence[str]) -> int:
    if len(argv) >= 4 and argv[1] == "--docker-cli":
        docker = json.loads(argv[2])
        return subprocess.run(
            [*docker, *_wsl_client_file_args(argv[4:], argv[3])], check=False
        ).returncode

    ensure_only = len(argv) == 3 and argv[1] == "--ensure-container"
    if not ensure_only and len(argv) < 3:
        print(
            "usage: windows_container_action_wrapper.py [--ensure-container] CONFIG_JSON [REAL_TOOL [ARGS...]]",
            file=sys.stderr,
        )
        return 2

    config_path = pathlib.Path(argv[2] if ensure_only else argv[1])
    try:
        config = _read_config(config_path)
    except (OSError, ValueError) as exc:
        print(
            f"ERROR: could not read container action config {config_path} ({exc}); "
            "refusing to run the build tool natively",
            file=sys.stderr,
        )
        return 2

    config = prepare_runner_config(config)

    if ensure_only:
        _ensure_preflight_container(config, config_path)
        return 0

    return _run_in_container(config, config_path, argv[2], argv[3:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
