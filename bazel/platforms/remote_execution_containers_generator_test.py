from __future__ import annotations

import importlib.util
import pathlib
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


if __name__ == "__main__":
    unittest.main()
