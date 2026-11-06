"""Container runtimes for package tests, independent of the Docker API."""

import os
import subprocess
import tempfile
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any, Protocol

CONTAINER_TIMEOUT_SECONDS = 10 * 60
PODMAN_TASK_LABEL = "org.mongodb.package-test.task"


class ContainerError(RuntimeError):
    """A container runtime operation failed."""


class RunningContainer(Protocol):
    def logs(self) -> Iterator[bytes]: ...

    def wait(self) -> int: ...

    def remove(self) -> None: ...


class ContainerRuntime(Protocol):
    def login(self, username: str, password: str) -> None: ...

    def pull(self, image: str) -> str: ...

    def run(
        self, image: str, command: list[str], volumes: list[str], environment: dict[str, str]
    ) -> RunningContainer: ...

    def close(self) -> None: ...


class ManagedContainer:
    """Allow timeout cleanup and worker cleanup to remove the same container safely."""

    def __init__(self, container: RunningContainer):
        self.container = container
        self._lock = threading.Lock()
        self._removed = False

    def logs(self) -> Iterator[bytes]:
        yield from self.container.logs()

    def wait(self) -> int:
        return self.container.wait()

    def remove(self) -> None:
        with self._lock:
            if not self._removed:
                self.container.remove()
                self._removed = True


class ContainerTestRuntime:
    """Own one test's containers without closing the runtime shared by other tests."""

    def __init__(self, runtime: ContainerRuntime):
        self.runtime = runtime
        self._lock = threading.Lock()
        self._closed = False
        self._containers: list[ManagedContainer] = []

    def _check_active(self) -> None:
        with self._lock:
            if self._closed:
                raise ContainerError("Package test was canceled")

    def login(self, username: str, password: str) -> None:
        self._check_active()
        self.runtime.login(username, password)

    def pull(self, image: str) -> str:
        self._check_active()
        return self.runtime.pull(image)

    def run(
        self, image: str, command: list[str], volumes: list[str], environment: dict[str, str]
    ) -> ManagedContainer:
        self._check_active()
        container = ManagedContainer(self.runtime.run(image, command, volumes, environment))
        with self._lock:
            canceled = self._closed
            self._containers.append(container)
        # Container creation can finish after the timeout thread has called close().
        if canceled:
            container.remove()
            raise ContainerError("Package test was canceled")
        return container

    def close(self) -> None:
        with self._lock:
            self._closed = True
            containers = self._containers.copy()
        errors = []
        for container in containers:
            try:
                container.remove()
            except ContainerError as exc:
                errors.append(str(exc))
        if errors:
            raise ContainerError("; ".join(errors))


class DockerContainer:
    def __init__(self, container: Any):
        self.container = container

    def logs(self) -> Iterator[bytes]:
        from docker.errors import DockerException

        try:
            yield from self.container.logs(stream=True)
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc

    def wait(self) -> int:
        from docker.errors import DockerException

        try:
            return self.container.wait()["StatusCode"]
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc

    def remove(self) -> None:
        from docker.errors import DockerException

        try:
            self.container.remove(force=True)
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc


class DockerRuntime:
    def __init__(self, client: Any = None):
        import docker

        try:
            self.client = client if client is not None else docker.from_env()
        except docker.errors.DockerException as exc:
            raise ContainerError(str(exc)) from exc

    def login(self, username: str, password: str) -> None:
        from docker.errors import DockerException

        try:
            self.client.login(username=username, password=password)
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc

    def pull(self, image: str) -> str:
        from docker.errors import DockerException

        try:
            self.client.images.pull(image)
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc
        return image

    def run(
        self, image: str, command: list[str], volumes: list[str], environment: dict[str, str]
    ) -> DockerContainer:
        from docker.errors import DockerException

        try:
            container = self.client.containers.run(
                image,
                command=command,
                detach=True,
                user="root",
                volumes=volumes,
                environment=environment,
            )
        except DockerException as exc:
            raise ContainerError(str(exc)) from exc
        # Retain the container until its exit code has been read, then remove it explicitly.
        return DockerContainer(container)

    def close(self) -> None:
        self.client.close()


def _podman(
    arguments: list[str],
    *,
    input: str | None = None,
    timeout: int = 300,
    env: dict[str, str] | None = None,
) -> str:
    try:
        result = subprocess.run(
            ["podman", *arguments],
            check=True,
            capture_output=True,
            text=True,
            input=input,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        raise ContainerError(f"podman {arguments[0]} timed out after {timeout} seconds") from exc
    except subprocess.CalledProcessError as exc:
        raise ContainerError(f"podman {arguments[0]} failed: {(exc.stderr or '').strip()}") from exc
    except OSError as exc:
        raise ContainerError(str(exc)) from exc
    return result.stdout.strip()


class PodmanContainer:
    def __init__(self, container_id: str, runtime_environment: dict[str, str] | None = None):
        self.container_id = container_id
        self.runtime_environment = runtime_environment

    def logs(self) -> Iterator[bytes]:
        offset = 0
        try:
            while True:
                # Podman 4.9 on the native RHEL 8 IBM hosts can keep `logs --follow`
                # blocked after exit. Inspection refreshes the stored container state;
                # finite log snapshots let us finish without relying on that follower.
                status = _podman(
                    ["inspect", "--format", "{{.State.Status}}", self.container_id],
                    env=self.runtime_environment,
                )
                stopped = status in ("stopped", "exited")
                with subprocess.Popen(
                    ["podman", "logs", self.container_id],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    env=self.runtime_environment,
                ) as process:
                    try:
                        output, _ = process.communicate(timeout=60)
                    except subprocess.TimeoutExpired as exc:
                        process.kill()
                        process.communicate()
                        raise ContainerError("podman logs timed out after 60 seconds") from exc
                    if process.returncode != 0:
                        raise ContainerError(
                            f"podman logs failed: {output.decode(errors='replace').strip()}"
                        )
                if len(output) < offset:
                    raise ContainerError("Container logs were truncated during collection")
                end = len(output) if stopped else output.rfind(b"\n") + 1
                yield from output[offset:end].splitlines(keepends=True)
                offset = end
                if stopped:
                    return
                time.sleep(1)
        except OSError as exc:
            raise ContainerError(str(exc)) from exc

    def wait(self) -> int:
        output = _podman(["wait", self.container_id], env=self.runtime_environment)
        try:
            return int(output)
        except ValueError as exc:
            raise ContainerError(f"Invalid podman wait exit code: {output!r}") from exc

    def remove(self) -> None:
        _podman(
            ["rm", "--force", "--time=0", self.container_id],
            timeout=30,
            env=self.runtime_environment,
        )


class PodmanRuntime:
    def __init__(self):
        self.auth_directory: tempfile.TemporaryDirectory[str] | None = None
        task_id = os.environ.get("task_id")
        self.task_label = f"{task_id}-{os.environ.get('execution', '0')}" if task_id else None
        self.credentials_directory: Path | None = None
        self.runtime_environment: dict[str, str] | None = None
        if state_directory := os.environ.get("MONGO_PACKAGE_TEST_STATE_DIR"):
            if self.task_label is None:
                raise ContainerError("Podman task state requires an Evergreen task ID")
            state = Path(state_directory)
            try:
                if (state / "active").read_text() != self.task_label + "\n":
                    raise ContainerError("Podman package-test state belongs to another execution")
                self.runtime_environment = dict(os.environ)
                for name in ("XDG_RUNTIME_DIR", "CONTAINERS_STORAGE_CONF", "CONTAINERS_CONF"):
                    value = (state / name).read_text()
                    if not Path(value).resolve().is_relative_to(state.resolve()):
                        raise ContainerError(
                            f"Podman {name} is outside this task's state directory"
                        )
                    self.runtime_environment[name] = value
            except OSError as exc:
                raise ContainerError(
                    f"Podman package-test preparation has not completed: {exc}"
                ) from exc
            for name in ("TMPDIR", "TMP", "TEMP"):
                self.runtime_environment[name] = self.runtime_environment["XDG_RUNTIME_DIR"]
            self.runtime_environment.pop("REGISTRY_AUTH_FILE", None)
            self.credentials_directory = state / "credentials"
            self.credentials_directory.mkdir(mode=0o700, exist_ok=True)
        # All package commands must use the context checked by task preparation.
        _podman(["info"], env=self.runtime_environment)

    def login(self, username: str, password: str) -> None:
        # Keep credentials local to this task, even on shared static IBM hosts.
        self.close()
        self.auth_directory = tempfile.TemporaryDirectory(
            prefix="package-test-podman-", dir=self.credentials_directory
        )
        _podman(
            [
                "login",
                *self._auth_arguments(),
                "--username",
                username,
                "--password-stdin",
                "docker.io",
            ],
            input=password,
            env=self.runtime_environment,
        )

    def _auth_arguments(self) -> list[str]:
        if self.auth_directory is None:
            return []
        return ["--authfile", str(Path(self.auth_directory.name) / "auth.json")]

    def pull(self, image: str) -> str:
        # Fully qualify Docker Hub names to avoid Podman's interactive short-name resolution.
        if "/" not in image:
            image = "docker.io/library/" + image
        elif not any(character in image.split("/", 1)[0] for character in (".", ":")):
            image = "docker.io/" + image
        _podman(["pull", *self._auth_arguments(), image], env=self.runtime_environment)
        return image

    def run(
        self, image: str, command: list[str], volumes: list[str], environment: dict[str, str]
    ) -> PodmanContainer:
        arguments = [
            "run",
            "--detach",
            "--user=root",
            "--pull=never",
            # Conmon enforces this even if the package-test Python process is killed.
            f"--timeout={CONTAINER_TIMEOUT_SECONDS}",
            # Package tests need access to the mounted scripts, logs, and custom libraries.
            # Disabling labeling avoids relabeling shared Evergreen checkout/entitlement files.
            "--security-opt=label=disable",
        ]
        if self.task_label is not None:
            arguments.extend(["--label", f"{PODMAN_TASK_LABEL}={self.task_label}"])
        for volume in volumes:
            arguments.extend(["--volume", volume])
        for name, value in environment.items():
            arguments.extend(["--env", f"{name}={value}"])
        arguments.extend([image, *command])
        return PodmanContainer(
            _podman(arguments, env=self.runtime_environment), self.runtime_environment
        )

    def close(self) -> None:
        if self.auth_directory is not None:
            self.auth_directory.cleanup()
            self.auth_directory = None


def create_container_runtime(name: str) -> ContainerRuntime:
    if name == "docker":
        return DockerRuntime()
    if name == "podman":
        return PodmanRuntime()
    raise ValueError(f"Unknown container runtime: {name}")
