#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import os
import pathlib
import subprocess
import sys
from collections.abc import Iterator, Sequence

_STALE_RUNTIME_MESSAGE = "invalid internal status"
_MANUAL_RECOVERY_MESSAGE = (
    "Refusing to run `podman system migrate` because it stops the containers currently "
    "running for this user. Run `podman system migrate` manually after verifying that "
    "stopping those containers is safe, then retry."
)
_MIGRATION_DISABLED_MESSAGE = (
    "Automatic Podman runtime migration is disabled by MONGO_BAZEL_PODMAN_AUTO_MIGRATE. "
    "Run `podman system migrate` manually after verifying that stopping running "
    "containers is safe, then retry."
)
_MIGRATION_FORCE_ENV = "MONGO_BAZEL_PODMAN_AUTO_MIGRATE_FORCE"


def _run(argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(argv),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def _emit(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)


def _is_podman(argv: Sequence[str]) -> bool:
    return bool(argv) and pathlib.Path(argv[0]).name == "podman"


def _is_stale_runtime_failure(result: subprocess.CompletedProcess[str]) -> bool:
    detail = f"{result.stdout}\n{result.stderr}".casefold()
    return result.returncode != 0 and _STALE_RUNTIME_MESSAGE in detail


@contextlib.contextmanager
def _recovery_lock() -> Iterator[None]:
    import fcntl

    lock_path = pathlib.Path("/tmp") / f"mongodb-podman-recovery-{os.getuid()}.lock"
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def _runtime_is_healthy(podman: str) -> tuple[bool, subprocess.CompletedProcess[str]]:
    result = _run([podman, "info"])
    return result.returncode == 0, result


def _migration_is_allowed() -> bool:
    return os.environ.get("MONGO_BAZEL_PODMAN_AUTO_MIGRATE", "1").strip() not in (
        "0",
        "false",
        "False",
    )


def _migration_force_is_allowed() -> bool:
    return os.environ.get(_MIGRATION_FORCE_ENV, "0").strip().casefold() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _has_running_containers(podman: str) -> tuple[bool | None, str]:
    """Report whether this user still has usable running containers.

    ``None`` means that Podman could not determine the answer. In particular,
    a stale pause process can make existing containers unreachable without
    stopping their workload processes, so a failed listing must not authorize
    a destructive migration by itself.
    """
    result = _run([podman, "ps", "--quiet"])
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return None, detail or f"exit code {result.returncode}"
    return bool(result.stdout.strip()), ""


def run_with_recovery(argv: Sequence[str], *, quiet_recovery: bool = False) -> int:
    if not argv:
        raise ValueError("a container runtime command is required")

    command = list(argv)
    first_result = _run(command)
    if not _is_podman(command) or not _is_stale_runtime_failure(first_result):
        _emit(first_result)
        return first_result.returncode

    if not quiet_recovery:
        _emit(first_result)
    podman = command[0]
    if not quiet_recovery:
        print(
            "Checking whether another process has recovered stale Podman runtime state.",
            file=sys.stderr,
        )

    with _recovery_lock():
        healthy, recheck = _runtime_is_healthy(podman)
        if not healthy:
            if not _is_stale_runtime_failure(recheck):
                _emit(recheck)
                return first_result.returncode

            if quiet_recovery:
                _emit(recheck)

            if not _migration_is_allowed():
                print(_MIGRATION_DISABLED_MESSAGE, file=sys.stderr)
                return first_result.returncode

            has_running_containers, container_check_detail = _has_running_containers(podman)
            if has_running_containers is None and not _migration_force_is_allowed():
                print(
                    "Refusing to run `podman system migrate` because Podman could not "
                    "determine whether this user has running containers: "
                    f"{container_check_detail}. Set {_MIGRATION_FORCE_ENV}=1 only when "
                    "this Podman runtime is isolated and stopping its containers is safe, "
                    "then retry.",
                    file=sys.stderr,
                )
                return first_result.returncode

            if has_running_containers:
                print(_MANUAL_RECOVERY_MESSAGE, file=sys.stderr)
                return first_result.returncode

            migrate = _run([podman, "system", "migrate"])
            if migrate.returncode != 0:
                _emit(migrate)
                print("`podman system migrate` failed.", file=sys.stderr)
                return first_result.returncode

            healthy, post_migration = _runtime_is_healthy(podman)
            if not healthy:
                _emit(post_migration)
                detail = (post_migration.stderr or post_migration.stdout or "").strip()
                print(
                    "Podman remained unavailable after `podman system migrate`: "
                    f"{detail or f'exit code {post_migration.returncode}'}",
                    file=sys.stderr,
                )
                return first_result.returncode

    retry_result = _run(command)
    _emit(retry_result)
    return retry_result.returncode


def main(argv: Sequence[str] | None = None) -> int:
    command = list(sys.argv[1:] if argv is None else argv)
    quiet_recovery = bool(command and command[0] == "--quiet-recovery")
    if quiet_recovery:
        command.pop(0)
    return run_with_recovery(command, quiet_recovery=quiet_recovery)


if __name__ == "__main__":
    sys.exit(main())
