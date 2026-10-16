"""Select and recover Linux Docker engines for host-side cross-build actions.

Windows routing is opt-in through the calling runner; importing this module does not
change Docker settings or start a service. An explicitly selected same-distribution
WSL engine remains strict, while automatic selection can try a native Linux context.
"""

from __future__ import annotations

import os
import pathlib
import platform
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Sequence

from bazel.toolchains.cc import container_action as shared

DOCKER_HOST_MODE_ENV = "MONGO_MACOS_CROSS_ACTION_DOCKER_HOST_MODE"

WSL_DOCKER_HOST_MODE = "wsl"

WSL_LOCAL_DOCKER_HOST_MODE = "wsl-local"

WSL_DOCKER_HOST_MODES = frozenset({WSL_DOCKER_HOST_MODE, WSL_LOCAL_DOCKER_HOST_MODE})

WSL_DISTRIBUTION_ENV = "MONGO_MACOS_CROSS_ACTION_WSL_DISTRIBUTION"

WSL_DOCKER_SOCKET_ENV = "MONGO_MACOS_CROSS_ACTION_WSL_DOCKER_SOCKET"

WSL_DOCKER_SOCKET_DEFAULT = "unix:///var/run/docker.sock"

WSL_LOCAL_DOCKER_START_TIMEOUT_SECONDS = 30

DOCKER_DESKTOP_HOST_MODE = "docker-desktop"

DOCKER_DESKTOP_SOCKET_MARKERS = ("/mnt/wsl/", "docker-desktop", "shared-docker")

WSL_TCP_DOCKER_HOST_DEFAULT = "tcp://127.0.0.1:2375"


def _docker_daemon_is_docker_desktop(docker: Sequence[str]) -> bool:
    """Returns whether a Linux Docker client is connected to Docker Desktop."""
    try:
        info = shared.run(
            [*docker, "info", "--format", "{{.OperatingSystem}}\t{{.Name}}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.TimeoutExpired):
        info = None

    if info is not None and info.returncode == 0:
        info_text = (info.stdout or "").strip().lower()
        if "docker desktop" in info_text or "docker-desktop" in info_text:
            return True

    socket = os.environ.get(WSL_DOCKER_SOCKET_ENV, WSL_DOCKER_SOCKET_DEFAULT)
    if not socket.startswith("unix://"):
        return False

    wsl = _wsl_exec_command(["readlink", "-f", socket.removeprefix("unix://")])
    if wsl is None:
        return False
    try:
        result = shared.run(wsl, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except (OSError, subprocess.TimeoutExpired):
        return False
    if result.returncode != 0:
        return False

    socket_target = (result.stdout or "").strip().lower()
    return any(marker in socket_target for marker in DOCKER_DESKTOP_SOCKET_MARKERS)


def _wsl_local_docker_ready(docker: Sequence[str]) -> bool:
    return shared.docker_info_os_type(docker) == "linux" and not _docker_daemon_is_docker_desktop(
        docker
    )


def _wsl_exec_command(command: Sequence[str]) -> list[str] | None:
    prefix = _wsl_command_prefix()
    if prefix is None:
        return None
    return [*prefix, "--exec", *command]


def _wsl_local_docker_start_commands() -> list[list[str]]:
    socket = os.environ.get(WSL_DOCKER_SOCKET_ENV, WSL_DOCKER_SOCKET_DEFAULT)
    daemon_command = (
        f"nohup dockerd --host={shlex.quote(socket)} "
        ">/tmp/mongo-wsl-dockerd.log 2>&1 </dev/null &"
    )
    commands = []
    for command in (
        ["sudo", "-n", "systemctl", "start", "docker"],
        ["sudo", "-n", "service", "docker", "start"],
        ["sudo", "-n", "sh", "-c", daemon_command],
        ["sh", "-c", daemon_command],
    ):
        wsl_command = _wsl_exec_command(command)
        if wsl_command is not None:
            commands.append(wsl_command)
    return commands


def _wsl_local_docker_restart_commands() -> list[list[str]]:
    commands = []
    for command in (
        ["sudo", "-n", "systemctl", "restart", "docker"],
        ["sudo", "-n", "service", "docker", "restart"],
    ):
        wsl_command = _wsl_exec_command(command)
        if wsl_command is not None:
            commands.append(wsl_command)
    return commands


def _ensure_wsl_local_docker_engine(docker: Sequence[str]) -> tuple[bool, str]:
    """Starts and verifies Docker Engine inside the selected WSL distribution."""
    wsl = _wsl_exec_command(["sh", "-c", "command -v dockerd"])
    if wsl is None:
        return False, "wsl.exe was not found"

    try:
        result = shared.run(wsl, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return False, detail or "dockerd is not installed in the WSL distribution"

    if _wsl_local_docker_ready(docker):
        return True, ""

    last_failure = ""
    for command in _wsl_local_docker_start_commands():
        try:
            result = shared.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except (OSError, subprocess.TimeoutExpired) as exc:
            last_failure = str(exc)
            continue

        if result.returncode != 0:
            last_failure = (result.stderr or result.stdout or "").strip()

        if _wsl_local_docker_ready(docker):
            return True, ""

    deadline = time.monotonic() + WSL_LOCAL_DOCKER_START_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if _wsl_local_docker_ready(docker):
            return True, ""
        time.sleep(1)

    return False, last_failure or "dockerd did not become ready"


def restart_wsl_local_docker_engine(docker: Sequence[str]) -> tuple[bool, str]:
    """Restarts a local Docker service so it observes newly mounted WSL drives."""
    last_failure = ""
    for command in _wsl_local_docker_restart_commands():
        try:
            result = shared.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except (OSError, subprocess.TimeoutExpired) as exc:
            last_failure = str(exc)
            continue

        if result.returncode != 0:
            last_failure = (result.stderr or result.stdout or "").strip()
            continue

        deadline = time.monotonic() + WSL_LOCAL_DOCKER_START_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            if _wsl_local_docker_ready(docker):
                return True, ""
            time.sleep(1)

    return False, last_failure or "Docker Engine did not become ready after restart"


def _wsl_local_docker_repair_instructions() -> str:
    distribution = os.environ.get(WSL_DISTRIBUTION_ENV)
    distribution_arg = f"--distribution {distribution} " if distribution else ""
    return (
        "Install and start Docker Engine in the same WSL distribution used by this build, "
        "then retry:\n"
        f"  wsl.exe {distribution_arg}--exec sudo apt-get install -y docker.io\n"
        f"  wsl.exe {distribution_arg}--exec sudo systemctl enable --now docker\n"
        "If systemd is unavailable, start it manually with:\n"
        f"  wsl.exe {distribution_arg}--exec sudo dockerd --host={WSL_DOCKER_SOCKET_DEFAULT}\n"
        "To select a different distribution, set "
        f"{WSL_DISTRIBUTION_ENV}."
    )


def _docker_context_names(docker: Sequence[str]) -> list[str]:
    try:
        result = shared.run(
            [*docker, "context", "ls", "--format", "{{.Name}}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode:
        return []
    return [name.strip() for name in (result.stdout or "").splitlines() if name.strip()]


def _wsl_command_prefix() -> list[str] | None:
    if platform.system() != "Windows":
        return None

    roots = [os.environ.get("SystemRoot", ""), r"C:\Windows"]
    for root in roots:
        if not root:
            continue
        wsl = pathlib.Path(root) / "System32" / "wsl.exe"
        if wsl.is_file():
            command = [str(wsl)]
            distribution = os.environ.get(WSL_DISTRIBUTION_ENV)
            if distribution:
                command.extend(["--distribution", distribution])
            return command
    return None


def _wsl_docker_command(*, local_engine: bool = False) -> list[str] | None:
    wsl = _wsl_command_prefix()
    if wsl is None:
        return None

    # ``wsl.exe <command> ...`` may route arguments through a shell. Cross
    # actions pass Windows environment values such as LIB, which contain
    # semicolons and must reach Docker without shell parsing.
    command = [*wsl, "--exec"]
    if local_engine:
        socket = os.environ.get(WSL_DOCKER_SOCKET_ENV, WSL_DOCKER_SOCKET_DEFAULT)
        # DOCKER_CONTEXT takes precedence over DOCKER_HOST in the Docker CLI.
        # Clear it so a WSL Docker Desktop/default context cannot redirect this
        # command away from the daemon in the selected distribution.
        command.extend(["env", f"DOCKER_HOST={socket}", "DOCKER_CONTEXT="])
    command.append("docker")
    return command


def _tcp_wsl_docker_command(docker: Sequence[str]) -> list[str] | None:
    """Selects the WSL Docker Engine exposed over a TCP endpoint.

    WSL distribution registration is per-user, so an image whose distribution
    was installed as Administrator (for example the Evergreen Windows images)
    hides it from other users even while the distribution's keepalive daemon
    keeps serving (DEVPROD-33175). That daemon listens on a TCP endpoint any
    Windows user can reach, and because it runs inside the WSL VM it resolves
    /mnt/<drive> bind sources through drvfs exactly like the same-distro
    engine.
    """
    previous_host = os.environ.get("DOCKER_HOST")
    # CLI routing overrides the environment and could make a TCP probe report
    # success against a native daemon. Leave these commands and non-TCP hosts
    # untouched so the native fallback retains its Windows bind-source policy.
    if _has_docker_context(docker) or any(
        argument in {"-H", "--host"} or argument.startswith("--host=") or argument.startswith("-H")
        for argument in docker[1:]
    ):
        return None
    if previous_host and not previous_host.startswith("tcp://"):
        return None
    previous_context = os.environ.get("DOCKER_CONTEXT")
    os.environ["DOCKER_HOST"] = previous_host or WSL_TCP_DOCKER_HOST_DEFAULT
    # DOCKER_CONTEXT takes precedence over DOCKER_HOST in the Docker CLI.
    os.environ.pop("DOCKER_CONTEXT", None)
    if shared.docker_info_os_type(docker) != "linux":
        # Restore the caller's Docker routing so engine selection can fall
        # through to the remaining candidates.
        if previous_host is None:
            os.environ.pop("DOCKER_HOST", None)
        else:
            os.environ["DOCKER_HOST"] = previous_host
        if previous_context is not None:
            os.environ["DOCKER_CONTEXT"] = previous_context
        return None
    os.environ[DOCKER_HOST_MODE_ENV] = WSL_DOCKER_HOST_MODE
    print(
        f"INFO: using the WSL Docker Engine over TCP ({os.environ['DOCKER_HOST']})",
        file=sys.stderr,
    )
    return list(docker)


def _has_docker_context(docker: Sequence[str]) -> bool:
    return any(argument == "--context" or argument.startswith("--context=") for argument in docker)


def _docker_desktop_engine_switch_commands(docker: Sequence[str]) -> list[list[str]]:
    commands: list[list[str]] = [[*docker, "desktop", "engine", "use", "linux"]]
    if platform.system() != "Windows":
        return commands

    install_roots = [
        os.environ.get("ProgramW6432", ""),
        os.environ.get("ProgramFiles", ""),
        os.environ.get("LOCALAPPDATA", ""),
        r"C:\Program Files",
    ]
    docker_cli_candidates = [shutil.which("DockerCli.exe")]
    docker_executable = shutil.which(docker[0]) if docker else None
    if docker_executable:
        docker_executable_path = pathlib.Path(docker_executable)
        docker_cli_candidates.extend(
            parent / "DockerCli.exe" for parent in docker_executable_path.parents
        )
    for root in install_roots:
        if not root:
            continue
        root_path = pathlib.Path(root)
        docker_cli_candidates.extend(
            [
                root_path / "Docker" / "Docker" / "DockerCli.exe",
                root_path / "Docker" / "DockerCli.exe",
                root_path / "Docker" / "Docker" / "resources" / "bin" / "DockerCli.exe",
                root_path / "Docker" / "resources" / "bin" / "DockerCli.exe",
            ]
        )

    seen: set[str] = set()
    for docker_cli in docker_cli_candidates:
        if docker_cli is None:
            continue
        docker_cli_path = pathlib.Path(docker_cli)
        if not docker_cli_path.is_file() or str(docker_cli_path) in seen:
            continue
        seen.add(str(docker_cli_path))
        commands.append([str(docker_cli_path), "-SwitchLinuxEngine"])
    return commands


def _switch_docker_desktop_to_linux(docker: Sequence[str]) -> list[str] | None:
    switched = False
    last_failure = ""
    for command in _docker_desktop_engine_switch_commands(docker):
        try:
            result = shared.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            last_failure = str(exc)
            continue
        if result.returncode == 0:
            switched = True
            break
        last_failure = (result.stderr or result.stdout or "").strip()
    if not switched:
        detail = f": {last_failure}" if last_failure else ""
        print(
            "WARNING: could not switch Docker Desktop to Linux containers"
            f"{detail}; trying the WSL Docker engine",
            file=sys.stderr,
        )
        return None

    candidates = [[*docker, "--context", "default"], list(docker)]
    for _ in range(15):
        for candidate in candidates:
            if shared.docker_info_os_type(candidate) == "linux":
                os.environ[DOCKER_HOST_MODE_ENV] = DOCKER_DESKTOP_HOST_MODE
                print("INFO: switched Docker Desktop to Linux containers", file=sys.stderr)
                return candidate
        time.sleep(1)
    return None


def select_linux_docker_command(docker: Sequence[str]) -> list[str] | None:
    """Selects a Docker command connected to a Linux container engine."""
    requested_mode = os.environ.get(DOCKER_HOST_MODE_ENV, "").lower()
    local_wsl_failure = ""

    # An explicitly requested WSL engine must be tried first. The default
    # Windows cross mode prefers Docker Engine in the same WSL distribution as
    # the drvfs mounts, while native Docker Desktop contexts remain available
    # as a fallback.
    if requested_mode == WSL_DOCKER_HOST_MODE:
        wsl_docker = _wsl_docker_command()
        if wsl_docker is not None and shared.docker_info_os_type(wsl_docker) == "linux":
            os.environ[DOCKER_HOST_MODE_ENV] = WSL_DOCKER_HOST_MODE
            print("INFO: using Linux Docker engine through WSL", file=sys.stderr)
            return wsl_docker

    # The default Windows cross configuration uses Docker Engine in the same
    # WSL distribution as the drvfs mounts. A Docker daemon in another WSL
    # distro or VM cannot see /mnt/<drive>, even when the WSL client can.
    if platform.system() == "Windows" and requested_mode in {"", WSL_LOCAL_DOCKER_HOST_MODE}:
        local_wsl_docker = _wsl_docker_command(local_engine=True)
        if local_wsl_docker is not None:
            ready, local_wsl_failure = _ensure_wsl_local_docker_engine(local_wsl_docker)
            if ready:
                os.environ[DOCKER_HOST_MODE_ENV] = WSL_LOCAL_DOCKER_HOST_MODE
                print("INFO: using Docker Engine in the same WSL distribution", file=sys.stderr)
                return local_wsl_docker
        else:
            local_wsl_failure = "wsl.exe was not found"

        if requested_mode == WSL_LOCAL_DOCKER_HOST_MODE:
            detail = f"{local_wsl_failure}\n" if local_wsl_failure else ""
            print(
                "ERROR: same-distro WSL Docker Engine is unavailable.\n"
                f"{detail}{_wsl_local_docker_repair_instructions()}",
                file=sys.stderr,
            )
            return None

        # The same-distro engine is only a preference in the default mode. When
        # this user cannot see a WSL distribution, the baked distribution's
        # keepalive daemon is still reachable over TCP.
        tcp_docker = _tcp_wsl_docker_command(docker)
        if tcp_docker is not None:
            return tcp_docker

    if requested_mode != WSL_DOCKER_HOST_MODE and shared.docker_info_os_type(docker) == "linux":
        return list(docker)

    if _has_docker_context(docker):
        contexts = []
    else:
        contexts = []
        for context in (
            os.environ.get("MONGO_MACOS_CROSS_ACTION_DOCKER_CONTEXT"),
            os.environ.get("DOCKER_CONTEXT"),
            "desktop-linux",
            *_docker_context_names(docker),
        ):
            if context and context not in contexts:
                contexts.append(context)

    for context in contexts:
        candidate = [*docker, "--context", context]
        if shared.docker_info_os_type(candidate) == "linux":
            # Docker Desktop's Linux context is accessed by the native Windows
            # CLI, so its bind-mount source paths must remain Windows paths.
            os.environ[DOCKER_HOST_MODE_ENV] = DOCKER_DESKTOP_HOST_MODE
            print(f"INFO: using Linux Docker context: {context}", file=sys.stderr)
            return candidate

    if not _has_docker_context(docker):
        switched = _switch_docker_desktop_to_linux(docker)
        if switched is not None:
            return switched

    if requested_mode != DOCKER_DESKTOP_HOST_MODE:
        if local_wsl_failure:
            print(
                "ERROR: the default Windows cross mode requires Docker Engine in the same "
                "WSL distribution so bind mounts expose Bazel outputs.\n"
                f"{local_wsl_failure}\n{_wsl_local_docker_repair_instructions()}",
                file=sys.stderr,
            )
            return None

        wsl_docker = _wsl_docker_command()
        if wsl_docker is not None and shared.docker_info_os_type(wsl_docker) == "linux":
            os.environ[DOCKER_HOST_MODE_ENV] = WSL_DOCKER_HOST_MODE
            print("INFO: using Linux Docker engine through WSL", file=sys.stderr)
            return wsl_docker

    print(
        "ERROR: Windows cross actions require a Linux Docker engine, but the active Docker "
        "engine is not Linux. Switch Docker Desktop to Linux containers or configure a Linux "
        "Docker context (for example, desktop-linux).",
        file=sys.stderr,
    )
    return None


def start_container_with_recovery(docker: Sequence[str], start: Callable[[], None]) -> bool:
    """Retry one failed startup, recovering the selected local WSL engine if needed."""
    try:
        start()
        return True
    except (SystemExit, subprocess.TimeoutExpired):
        if (
            platform.system() != "Windows"
            or os.environ.get(DOCKER_HOST_MODE_ENV) not in WSL_DOCKER_HOST_MODES
        ):
            raise
        if (
            os.environ.get("DOCKER_HOST", "").startswith("tcp://")
            and shared.docker_info_os_type(docker) == "linux"
        ):
            start()
            return True
        restarted, detail = restart_wsl_local_docker_engine(docker)
        if not restarted:
            print(f"ERROR: could not restart the WSL Docker engine: {detail}", file=sys.stderr)
            return False
        start()
        return True
