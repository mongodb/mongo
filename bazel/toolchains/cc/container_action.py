"""Docker primitives and supervised action transport shared by cross-build runners.

Callers supply the resolved container working directory and host scratch root.
Container startup, mount policy, and tool preparation remain in platform runners.
"""

from __future__ import annotations

import contextlib
import dataclasses
import errno
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
from collections.abc import Iterator, Sequence

try:
    import fcntl
except ImportError:
    fcntl = None

try:
    import msvcrt
except ImportError:
    msvcrt = None

# ``docker exec`` reserves these exit codes for failures to start the requested
# command: 125 is a Docker/daemon error, 126 means the command could not be
# invoked, and 127 means it could not be found. A tool that starts successfully
# never returns them, except when its dynamic loader cannot find a shared
# library (also 127). Any of them means the failure is in the container setup
# rather than in the tool's own work, so collect container-side diagnostics.
CONTAINER_EXEC_FAILURE_CODES = frozenset({125, 126, 127})
# ``docker exec`` streams a command's output over a hijacked HTTP connection and
# reads its exit status once that stream ends. The TCP relay in front of the WSL
# Docker Engine on the Windows CI images closes the stream early: the output is
# lost and the CLI reports an exit status while the command is still running.
# Windows actions therefore start their command detached (``docker exec -d``),
# redirect its output to a container-side file, have the shell record the exit
# status next to it, and copy those files back to the host.
ACTION_OUTPUT_LOG_ENV = "MONGO_CROSS_ACTION_OUTPUT_LOG"
ACTION_OUTPUT_LOG_DIRNAME = "action-output"
ACTION_EXIT_FILE_SUFFIX = ".exit"
# The status and log files live on the container's own filesystem and are read
# back with ``docker cp`` (a plain HTTP request), so this channel works even when
# the daemon cannot see the host's bind-mount sources.
CONTAINER_ACTION_STATE_DIR = pathlib.PurePosixPath("/tmp/mongo-cross-actions")
# The action environment replaces PATH with the toolchain's relative directories, so the
# shell must not rely on PATH to find coreutils.
ACTION_HEARTBEAT_SUFFIX = ".heartbeat"
ACTION_CANCEL_SUFFIX = ".cancel"
ACTION_LEASE_SECONDS = 30
ACTION_HEARTBEAT_INTERVAL_SECONDS = 5
ACTION_TERMINATION_GRACE_SECONDS = 1
# Bash job control gives only this tool and its descendants a fresh process group. The
# supervisor stays outside that group so it can stop and reap the tool before acknowledging
# cancellation. A renewable lease also stops actions when Windows terminates the host runner
# without giving Python a chance to run a signal handler or finally block.
ACTION_OUTPUT_CAPTURE_SCRIPT = f"""
log="${ACTION_OUTPUT_LOG_ENV}"
/bin/mkdir -p "${{log%/*}}" || exit 125
set -m
"$0" "$@" >"$log" 2>&1 &
tool_pid=$!
set +m
terminate_tool() {{
    trap '' INT TERM HUP
    kill -TERM -- "-$tool_pid" 2>/dev/null || true
    /bin/sleep {ACTION_TERMINATION_GRACE_SECONDS}
    kill -KILL -- "-$tool_pid" 2>/dev/null || true
    wait "$tool_pid" 2>/dev/null || true
}}
trap 'terminate_tool; printf "125\\n" >"$log{ACTION_EXIT_FILE_SUFFIX}"; exit 125' INT TERM HUP
heartbeat=""
renewed=$SECONDS
while kill -0 "$tool_pid" 2>/dev/null; do
    current=""
    if [ -f "$log{ACTION_HEARTBEAT_SUFFIX}" ]; then
        IFS= read -r current <"$log{ACTION_HEARTBEAT_SUFFIX}" || true
    fi
    if [ -n "$current" ] && [ "$current" != "$heartbeat" ]; then
        heartbeat="$current"
        renewed=$SECONDS
    fi
    if [ -f "$log{ACTION_CANCEL_SUFFIX}" ] || (( SECONDS - renewed >= {ACTION_LEASE_SECONDS} )); then
        terminate_tool
        printf "125\\n" >"$log{ACTION_EXIT_FILE_SUFFIX}"
        exit 125
    fi
    /bin/sleep 0.1
done
wait "$tool_pid"
status=$?
# A tool that finishes before a background child must not leave that child in the container.
kill -KILL -- "-$tool_pid" 2>/dev/null || true
printf '%s\\n' "$status" >"$log{ACTION_EXIT_FILE_SUFFIX}"
"""
ACTION_COMPLETION_TIMEOUT_ENV = "MONGO_CROSS_ACTION_TIMEOUT_SECONDS"
ACTION_COMPLETION_TIMEOUT_DEFAULT_SECONDS = 10800.0
ACTION_COMPLETION_POLL_SECONDS = 0.25
DOCKER_REQUEST_TIMEOUT_SECONDS = 30.0
ACTION_LOG_TIMEOUT_SECONDS = 5.0
CONTAINER_IDENTITY_CHECK_INTERVAL_POLLS = 8


def docker_info_os_type(docker: Sequence[str]) -> str:
    try:
        result = run(
            [*docker, "info", "--format", "{{.OSType}}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    if result.returncode:
        return ""
    return (result.stdout or "").strip().lower()


def run(
    args: Sequence[str], *, stdout=None, stderr=None, timeout=DOCKER_REQUEST_TIMEOUT_SECONDS
) -> subprocess.CompletedProcess:
    # Tool output is replayed rather than parsed, so never fail on bytes that the
    # host locale cannot decode (Windows console code pages are not UTF-8).
    return subprocess.run(
        args,
        check=False,
        stdout=stdout,
        stderr=stderr,
        text=True,
        errors="replace",
        timeout=timeout,
    )


def _request_timeout(deadline: float) -> float:
    """Bound requests so polling and lease renewal cannot be blocked by the Docker CLI."""
    return max(0.001, min(ACTION_HEARTBEAT_INTERVAL_SECONDS, deadline - time.monotonic()))


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", value)


def container_running(docker: Sequence[str], name: str) -> bool:
    result = run(
        [*docker, "inspect", "-f", "{{.State.Running}}", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0 and result.stdout.strip() == "true"


def container_exists(docker: Sequence[str], name: str) -> bool:
    return (
        run(
            [*docker, "inspect", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ).returncode
        == 0
    )


@contextlib.contextmanager
def exclusive_file_lock(lock_path: pathlib.Path):
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
            lock_mode = getattr(msvcrt, "LK_NBLCK", None)
            if lock_mode is None:
                lock_mode = msvcrt.LK_LOCK
            while True:
                try:
                    msvcrt.locking(lock_file.fileno(), lock_mode, 1)
                    break
                except OSError as exc:
                    if exc.errno not in {
                        errno.EACCES,
                        errno.EAGAIN,
                        errno.EDEADLK,
                    } and getattr(exc, "winerror", None) not in {33, 36}:
                        raise
                    time.sleep(0.1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            return

        yield


def _run_helper_container(
    docker: Sequence[str], image: str, command: Sequence[str], timeout: float = 120.0
) -> tuple[int, str]:
    """Runs a short privileged helper container and returns its exit status and logs.

    The container is started detached and observed through ``docker inspect`` and
    ``docker logs``, which are plain HTTP requests: the attached ``docker run``
    stream is not usable through the TCP relay in front of the WSL Docker Engine.
    """
    name = safe_name(f"mongo_cross_helper_{uuid.uuid4().hex[:12]}")
    deadline = time.monotonic() + timeout
    try:
        started = run(
            [*docker, "run", "-d", "--name", name, "--privileged", "--pid=host", image, *command],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=max(0.001, min(DOCKER_REQUEST_TIMEOUT_SECONDS, timeout)),
        )
        if started.returncode != 0:
            return started.returncode or 125, (
                (started.stderr or "") + (started.stdout or "")
            ).strip()
        while time.monotonic() < deadline:
            inspect = run(
                [*docker, "inspect", "-f", "{{.State.Status}} {{.State.ExitCode}}", name],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=_request_timeout(deadline),
            )
            fields = (inspect.stdout or "").split()
            if inspect.returncode == 0 and len(fields) == 2 and fields[0] in {"exited", "dead"}:
                try:
                    exit_code = int(fields[1])
                except ValueError:
                    exit_code = 125
                logs = run(
                    [*docker, "logs", name],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=_request_timeout(deadline),
                )
                if logs.returncode:
                    return (
                        125,
                        f"could not retrieve logs from helper container {name}: {logs.stdout}",
                    )
                return exit_code, (logs.stdout or "").strip()
            time.sleep(min(ACTION_COMPLETION_POLL_SECONDS, max(0, deadline - time.monotonic())))
        return 125, f"helper container {name} did not finish within {timeout:g}s"
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 125, f"Docker request for helper container {name} failed: {exc}"
    finally:
        try:
            run(
                [*docker, "rm", "-f", name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=ACTION_HEARTBEAT_INTERVAL_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass


def mount_windows_drives_in_docker_vm(
    docker: Sequence[str], image: str, drives: Sequence[str], prefix: str
) -> tuple[bool, str]:
    """Mounts Windows drives inside the WSL distribution that runs the Docker Engine.

    On the Windows CI images the only Linux engine is a dockerd inside a WSL
    distribution registered to another user, reached over TCP. That distribution
    may not have the host drives automounted, and Docker then silently bind-mounts
    empty directories for ``/mnt/<drive>`` sources. ``wsl.exe`` cannot reach the
    distribution from this user, but a privileged container sharing the daemon's
    PID namespace can enter its init mount namespace and mount the drives
    through drvfs exactly as automount would have.
    """
    if not drives:
        return True, "no drives to mount"
    steps = []
    for drive in drives:
        mountpoint = f"{prefix}/{drive}"
        steps.append(
            f"mkdir -p {mountpoint}; "
            f"mountpoint -q {mountpoint} || mount -t drvfs {drive.upper()}: {mountpoint} || "
            f"echo mount of {drive.upper()}: at {mountpoint} failed"
        )
    script = "; ".join(steps) + "; grep -E ' /mnt/[a-z]+ ' /proc/mounts; ls -la /mnt"
    exit_code, logs = _run_helper_container(
        docker, image, ["nsenter", "-t", "1", "-m", "--", "sh", "-c", script]
    )
    ok = exit_code == 0 and "failed" not in logs
    return ok, f"exit {exit_code}\n{logs}"


def action_completion_timeout() -> float:
    value = os.environ.get(ACTION_COMPLETION_TIMEOUT_ENV, "")
    try:
        return float(value) if value else ACTION_COMPLETION_TIMEOUT_DEFAULT_SECONDS
    except ValueError:
        return ACTION_COMPLETION_TIMEOUT_DEFAULT_SECONDS


def _read_exit_file(exit_file: pathlib.Path) -> int | None:
    """Returns the recorded exit status, or ``None`` while the command is still running."""
    try:
        text = exit_file.read_text(encoding="utf-8", errors="replace").strip()
    except (OSError, subprocess.TimeoutExpired):
        return None
    if not text:
        # The shell has created the file but the status has not been flushed yet.
        return None
    try:
        return int(text.split()[0])
    except ValueError:
        return 125


@contextlib.contextmanager
def _action_interrupt_handlers(*, cleanup: bool = False) -> Iterator[None]:
    """Let cancellation unwind into action cleanup instead of killing only the runner."""
    if threading.current_thread() is not threading.main_thread():
        yield
        return

    def interrupt(signum: int, _frame: object) -> None:
        if cleanup:
            # Cleanup must acknowledge termination or wait out the lease before returning.
            return
        if signum == signal.SIGINT:
            raise KeyboardInterrupt
        raise SystemExit(128 + signum)

    previous = {
        signum: signal.signal(signum, interrupt) for signum in (signal.SIGINT, signal.SIGTERM)
    }
    try:
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def _write_container_action_state(
    docker: Sequence[str],
    name: str,
    container_file: pathlib.PurePosixPath,
    host_dir: pathlib.Path,
    deadline: float,
) -> bool:
    """Publish a fresh lease or cancellation marker with an ordinary Docker HTTP request."""
    host_file = host_dir / container_file.name
    host_file.write_text(uuid.uuid4().hex + "\n", encoding="utf-8")
    result = run(
        [*docker, "cp", str(host_file), f"{name}:{container_file.as_posix()}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=_request_timeout(deadline),
    )
    return result.returncode == 0


@_action_interrupt_handlers(cleanup=True)
def _cancel_container_action(
    docker: Sequence[str],
    name: str,
    container_log: pathlib.PurePosixPath,
    host_dir: pathlib.Path,
) -> None:
    """Wait for this action's supervisor to stop the tool; never kill a shared container."""
    cancel_file = pathlib.PurePosixPath(str(container_log) + ACTION_CANCEL_SUFFIX)
    exit_file = pathlib.PurePosixPath(str(container_log) + ACTION_EXIT_FILE_SUFFIX)
    # If the Docker connection is lost, wait out the supervisor's lease before abandoning
    # the action. Otherwise the tool could still modify outputs after Bazel sees failure.
    deadline = time.monotonic() + ACTION_LEASE_SECONDS + ACTION_TERMINATION_GRACE_SECONDS + 2
    try:
        _write_container_action_state(docker, name, cancel_file, host_dir, deadline)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"WARNING: could not publish cancellation of {container_log}: {exc}", file=sys.stderr)
    while time.monotonic() < deadline:
        try:
            copied = _copy_from_container(docker, name, exit_file, host_dir, deadline)
            if copied is not None and _read_exit_file(copied) is not None:
                return
        except (OSError, subprocess.TimeoutExpired):
            pass
        time.sleep(ACTION_COMPLETION_POLL_SECONDS)
    print(
        f"WARNING: cancellation of {container_log} waited for the container lease to expire",
        file=sys.stderr,
    )


def run_container_command(
    docker: Sequence[str],
    name: str,
    container_cwd: pathlib.PurePosixPath,
    env_args: Sequence[str],
    command: Sequence[str],
    host_state_root: pathlib.Path,
    timeout: float | None = None,
) -> tuple[int, str, list[str]]:
    """Run an owned container action, cleaning it up when the host runner is interrupted."""
    with _action_interrupt_handlers():
        return _run_container_command_impl(
            docker, name, container_cwd, env_args, command, host_state_root, timeout
        )


def _run_container_command_impl(
    docker: Sequence[str],
    name: str,
    container_cwd: pathlib.PurePosixPath,
    env_args: Sequence[str],
    command: Sequence[str],
    host_state_root: pathlib.Path,
    timeout: float | None,
) -> tuple[int, str, list[str]]:
    """Runs ``command`` in the action container without relying on the exec stream.

    Returns the command's exit status, its combined output, and the ``docker``
    command line that launched it. The exit status is 125 when the exec could not
    be created or when the command did not report completion within the timeout.
    """
    host_dir, container_log = _action_state_paths(host_state_root)
    container_exit_file = container_log.with_name(container_log.name + ACTION_EXIT_FILE_SUFFIX)
    capture_environment, capture_command = _action_output_capture(container_log, command)
    launch = [
        *docker,
        "exec",
        "-d",
        "-w",
        container_cwd.as_posix(),
        *env_args,
        *capture_environment,
        name,
        *capture_command,
    ]
    launched = False
    completed = False
    preserve_log = False
    timeout = timeout if timeout is not None else action_completion_timeout()
    deadline = time.monotonic() + timeout
    try:
        # An interrupted CLI can start its detached exec before reporting success.
        launched = True
        started = run(
            launch,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=_request_timeout(deadline),
        )
        if started.returncode != 0:
            detail = ((started.stderr or "") + (started.stdout or "")).strip()
            return (
                started.returncode or 125,
                f"docker exec could not start {command[0]} in the container: "
                f"{detail or '<no detail>'}",
                launch,
            )

        container_state = _container_state(docker, name, deadline)
        polls = 0
        next_heartbeat = 0.0
        while True:
            exit_file = _copy_from_container(docker, name, container_exit_file, host_dir, deadline)
            exit_code = _read_exit_file(exit_file) if exit_file is not None else None
            if exit_code is not None:
                completed = True
                break
            polls += 1
            if polls % CONTAINER_IDENTITY_CHECK_INTERVAL_POLLS == 0:
                current_state = _container_state(docker, name, deadline)
                failure = None
                if current_state is None:
                    failure = "could not be inspected"
                elif not current_state.running:
                    failure = "stopped"
                elif container_state is not None:
                    if current_state.identity != container_state.identity:
                        failure = "was replaced"
                    elif current_state.started_at != container_state.started_at:
                        failure = "restarted"
                if failure is not None:
                    # Stop, restart, or replacement proves the old process is gone.
                    # A failed inspect may only mean connection loss, so wait out its lease.
                    completed = current_state is not None
                    return (
                        125,
                        f"the action container {name} {failure} while {command[0]} was "
                        "running; retry the action",
                        launch,
                    )
                container_state = current_state
            if time.monotonic() >= deadline:
                _cancel_container_action(docker, name, container_log, host_dir)
                completed = True
                output = _collect_action_output(docker, name, container_log, host_dir)
                preserve_log = output is None
                output = (
                    output
                    if output is not None
                    else f"could not retrieve action output; log retained at {name}:{container_log}"
                )
                return (
                    125,
                    output.rstrip()
                    + f"\n{command[0]} did not report completion within {timeout:g}s; "
                    f"its exit status file {container_exit_file} never appeared in the container",
                    launch,
                )
            if time.monotonic() >= next_heartbeat:
                _write_container_action_state(
                    docker,
                    name,
                    pathlib.PurePosixPath(str(container_log) + ACTION_HEARTBEAT_SUFFIX),
                    host_dir,
                    deadline,
                )
                next_heartbeat = time.monotonic() + ACTION_HEARTBEAT_INTERVAL_SECONDS
            time.sleep(ACTION_COMPLETION_POLL_SECONDS)

        output = _collect_action_output(docker, name, container_log, host_dir)
        preserve_log = output is None
        if preserve_log:
            return (
                125,
                f"could not retrieve action output (tool exited {exit_code}); log retained at {name}:{container_log}",
                launch,
            )
        return exit_code, output, launch
    except (OSError, subprocess.TimeoutExpired) as exc:
        preserve_log = True
        return (
            125,
            f"Docker request for {command[0]} failed: {exc}; log retained at {name}:{container_log}",
            launch,
        )
    finally:
        with _action_interrupt_handlers(cleanup=True):
            if launched and not completed:
                _cancel_container_action(docker, name, container_log, host_dir)
            if launched and not preserve_log:
                try:
                    run(
                        [
                            *docker,
                            "exec",
                            "-d",
                            name,
                            "/bin/rm",
                            "-f",
                            str(container_log),
                            str(container_exit_file),
                            str(container_log) + ACTION_HEARTBEAT_SUFFIX,
                            str(container_log) + ACTION_CANCEL_SUFFIX,
                        ],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=ACTION_HEARTBEAT_INTERVAL_SECONDS,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    pass
            shutil.rmtree(host_dir, ignore_errors=True)


@dataclasses.dataclass(frozen=True)
class _ContainerState:
    identity: str
    running: bool
    started_at: str


def _container_state(docker: Sequence[str], name: str, deadline: float) -> _ContainerState | None:
    """Inspect identity and lifetime together; restarting preserves a container's ID."""
    result = run(
        [*docker, "inspect", "-f", "{{.Id}} {{.State.Running}} {{.State.StartedAt}}", name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=_request_timeout(deadline),
    )
    if result.returncode != 0:
        return None
    fields = (result.stdout or "").split()
    if len(fields) != 3 or fields[1] not in {"true", "false"}:
        return None
    return _ContainerState(fields[0], fields[1] == "true", fields[2])


def _action_state_paths(
    host_state_root: pathlib.Path,
) -> tuple[pathlib.Path, pathlib.PurePosixPath]:
    """Returns a fresh host scratch directory and the container-side log path for one command.

    The log and its exit-status file are written to the container's own
    filesystem and copied out with ``docker cp``; the host directory only holds
    the copies while they are read.
    """
    identity = f"{os.getpid()}-{uuid.uuid4().hex}"
    host_dir = host_state_root / identity
    host_dir.mkdir(parents=True, exist_ok=True)
    return host_dir, CONTAINER_ACTION_STATE_DIR / f"{identity}.log"


def _copy_from_container(
    docker: Sequence[str],
    name: str,
    container_path: pathlib.PurePosixPath,
    host_dir: pathlib.Path,
    deadline: float,
) -> pathlib.Path | None:
    """Copies one file out of the container, returning the host copy or ``None`` if absent."""
    result = run(
        [*docker, "cp", f"{name}:{container_path.as_posix()}", str(host_dir)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=_request_timeout(deadline),
    )
    if result.returncode != 0:
        return None
    copied = host_dir / container_path.name
    return copied if copied.is_file() else None


def _collect_action_output(
    docker: Sequence[str],
    name: str,
    container_log: pathlib.PurePosixPath,
    host_dir: pathlib.Path,
) -> str | None:
    """Retry failed log requests; an empty log is different from a failed copy."""
    deadline = time.monotonic() + ACTION_LOG_TIMEOUT_SECONDS
    while True:
        try:
            log_file = _copy_from_container(docker, name, container_log, host_dir, deadline)
            if log_file is not None:
                return log_file.read_text(encoding="utf-8", errors="replace")
        except (OSError, subprocess.TimeoutExpired):
            pass
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        time.sleep(min(ACTION_COMPLETION_POLL_SECONDS, remaining))


def _action_output_capture(
    log_file: pathlib.PurePosixPath, command: Sequence[str]
) -> tuple[list[str], list[str]]:
    """Returns the exec environment and command that redirect output to ``log_file``."""
    return (
        ["-e", f"{ACTION_OUTPUT_LOG_ENV}={log_file.as_posix()}"],
        # Absolute: the exec environment may carry the action's PATH, which only lists
        # toolchain directories, and the container runtime resolves the command with it.
        ["/bin/bash", "-c", ACTION_OUTPUT_CAPTURE_SCRIPT, *command],
    )


def _read_and_remove_output_log(log_file: pathlib.Path) -> str | None:
    """Returns the redirected output, or ``None`` when the container never wrote it."""
    try:
        content = log_file.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return None
    except OSError as exc:
        return f"<could not read {log_file}: {exc}>"
    try:
        log_file.unlink()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return content
