from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


def _load_generator() -> types.ModuleType:
    retry_module = types.ModuleType("retry")
    retry_module.retry = lambda **_kwargs: lambda function: function
    path = pathlib.Path(__file__).with_name("remote_execution_containers_generator.py")
    spec = importlib.util.spec_from_file_location("remote_execution_containers_generator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, {"retry": retry_module}):
        spec.loader.exec_module(module)
    return module


generator = _load_generator()


class RemoteExecutionContainersGeneratorTest(unittest.TestCase):
    def test_rhel_images_build_all_supported_host_architectures(self) -> None:
        expected = (
            "linux/amd64",
            "linux/arm64/v8",
            "linux/ppc64le",
            "linux/s390x",
        )
        for distro in ("rhel8", "rhel9", "rhel10"):
            with self.subTest(distro=distro):
                self.assertEqual(expected, generator.platforms_for_distro(distro))

    def test_other_distros_keep_default_platforms(self) -> None:
        self.assertEqual(
            ("linux/amd64", "linux/arm64/v8"),
            generator.platforms_for_distro("debian12"),
        )

    def test_emulated_architectures_excludes_aarch64_host(self) -> None:
        self.assertEqual(
            ("amd64", "ppc64le", "s390x"),
            generator.emulated_architectures("aarch64"),
        )

    def test_emulated_architectures_excludes_x86_64_host(self) -> None:
        self.assertEqual(
            ("arm64", "ppc64le", "s390x"),
            generator.emulated_architectures("x86_64"),
        )

    def test_resolve_dockerfile_accepts_generated_lowercase_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            context = pathlib.Path(temp_dir)
            generated = context / "dockerfile"
            generated.touch()

            self.assertEqual(generated, generator.resolve_dockerfile(context / "Dockerfile"))

    def test_web_url_points_at_the_ecr_console_for_the_pushed_digest(self) -> None:
        digest = "sha256:" + "a" * 64
        self.assertEqual(
            f"https://gallery.ecr.aws/w3i8j1a8/devprod-build/{digest}",
            generator.web_url_for_image(f"public.ecr.aws/w3i8j1a8/devprod-build@{digest}"),
        )

    def test_create_buildx_builder_removes_stale_builder_before_each_attempt(self) -> None:
        with mock.patch.object(generator, "_log_subprocess_run") as run:
            generator.create_buildx_builder("test-builder")

        self.assertEqual(
            [
                mock.call(
                    ["docker", "buildx", "rm", "--force", "test-builder"],
                    check=False,
                ),
                mock.call(
                    [
                        "docker",
                        "buildx",
                        "create",
                        "--name",
                        "test-builder",
                        "--driver",
                        "docker-container",
                        "--use",
                        "--bootstrap",
                    ],
                    check=True,
                ),
            ],
            run.call_args_list,
        )


class ContinueOnErrorTest(unittest.TestCase):
    """--continue-on-error keeps a broken distro from discarding the distros behind it."""

    def _run_main(self, argv: list[str], failing_distro: str) -> tuple[int, dict, list[str]]:
        containers = {
            "debian12": {"dockerfile": "debian12/Dockerfile"},
            "ubuntu24": {"dockerfile": "ubuntu24/Dockerfile"},
        }
        built = []

        def fake_run(*args, **kwargs):
            command = args[0]
            if isinstance(command, list) and command[:3] == ["docker", "buildx", "build"]:
                distro = command[command.index("--tag") + 1].split(":")[-1].rsplit("-", 2)[0]
                built.append(distro)
                if distro == failing_distro:
                    raise subprocess.CalledProcessError(1, command)
            return mock.Mock(stdout="'[repo@sha256:abc123]'")

        with tempfile.TemporaryDirectory() as temp_dir:
            container_file = pathlib.Path(temp_dir) / "remote_execution_containers.bzl"
            container_file.write_text(f"REMOTE_EXECUTION_CONTAINERS = {containers!r}\n")
            with (
                mock.patch.object(sys, "argv", ["generator", "--skip-cleanup", *argv]),
                mock.patch.object(generator.os.path, "join", return_value=str(container_file)),
                mock.patch.object(generator, "resolve_dockerfile", side_effect=pathlib.Path),
                mock.patch.object(generator, "create_buildx_builder"),
                mock.patch.object(generator, "log_subprocess_run", side_effect=fake_run),
            ):
                exit_code = generator.main()

            written = {}
            exec(compile(container_file.read_text(), "containers", "exec"), {}, written)

        return exit_code, written["REMOTE_EXECUTION_CONTAINERS"], built

    def test_later_distros_still_update_after_a_failure(self) -> None:
        exit_code, containers, built = self._run_main(["--continue-on-error"], "debian12")

        self.assertEqual(["debian12", "ubuntu24"], built)
        self.assertEqual(1, exit_code, "the run must still fail the task")
        self.assertNotIn("container-url", containers["debian12"])
        self.assertEqual("docker://repo@sha256:abc123", containers["ubuntu24"]["container-url"])

    def test_failure_aborts_the_run_by_default(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_main([], "debian12")


if __name__ == "__main__":
    unittest.main()
