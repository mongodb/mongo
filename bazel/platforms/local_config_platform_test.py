"""Tests for the repository-generated Windows cross execution platform."""

import ast
import json
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


class WindowsCrossExecutionPlatformTest(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).with_name("local_config_platform.bzl")
        tree = ast.parse(path.read_text())
        helper = next(
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "_write_windows_cross_execution_platform"
        )
        self.namespace = {
            "json": SimpleNamespace(encode=json.dumps),
            "REMOTE_EXECUTION_CONTAINERS": {"ubuntu24": {}},
            "fail": lambda message: (_ for _ in ()).throw(ValueError(message)),
        }
        exec(compile(ast.Module(body=[helper], type_ignores=[]), str(path), "exec"), self.namespace)

    def test_selected_properties_override_parent_and_preserve_constraints(self):
        image = "docker://example.com/custom@sha256:abc"
        ctx = SimpleNamespace(
            os=SimpleNamespace(
                environ={
                    "MONGO_WINDOWS_CROSS_RBE_PLATFORM_IMAGE": image,
                    "MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO": "ubuntu24",
                    "MONGO_WINDOWS_CROSS_RBE_POOL": "custom-x64",
                }
            ),
            file=mock.Mock(),
        )
        self.namespace["_write_windows_cross_execution_platform"](ctx)
        native_platform = mock.Mock()
        exec(ctx.file.call_args.args[1], {"package": mock.Mock(), "platform": native_platform})
        platform = native_platform.call_args.kwargs
        self.assertEqual(
            platform["parents"], ["@//bazel/platforms:ubuntu24_amd64_windows_cross_exec"]
        )
        self.assertEqual(
            platform["exec_properties"],
            {"container-image": image, "Pool": "custom-x64", "dockerNetwork": "standard"},
        )

    def test_native_platform_generation_is_unchanged(self):
        ctx = SimpleNamespace(os=SimpleNamespace(environ={}), file=mock.Mock())
        self.namespace["_write_windows_cross_execution_platform"](ctx)
        ctx.file.assert_not_called()

    def test_invalid_distro_or_incompatible_pool_is_rejected(self):
        for overrides in (
            {"MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO": "unknown"},
            {"MONGO_WINDOWS_CROSS_RBE_POOL": "arm64"},
        ):
            ctx = SimpleNamespace(
                os=SimpleNamespace(
                    environ={
                        "MONGO_WINDOWS_CROSS_RBE_PLATFORM_IMAGE": "docker://image",
                        "MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO": "ubuntu24",
                        **overrides,
                    }
                ),
                file=mock.Mock(),
            )
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                self.namespace["_write_windows_cross_execution_platform"](ctx)


if __name__ == "__main__":
    unittest.main()
