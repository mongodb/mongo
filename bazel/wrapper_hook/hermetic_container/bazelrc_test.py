"""Tests for Bazel command inheritance in container execution planning."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from bazel.wrapper_hook.hermetic_container import bazelrc


class BazelrcTest(unittest.TestCase):
    def test_commands_inherit_execution_options_and_config_aliases(self):
        for command, inherits_test in (
            ("cquery", True),
            ("vendor", True),
            ("canonicalize-flags", False),
        ):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as directory:
                repo_root = pathlib.Path(directory)
                (repo_root / ".bazelrc").write_text(
                    "common --repo_env=COMMON=yes\n"
                    "build --remote_executor=\n"
                    "test --remote_executor=grpc://test-executor\n"
                    f"{command} --repo_env=COMMAND=yes\n"
                    "build:execution --repo_env=BUILD_CONFIG=yes\n"
                    "test:execution --repo_env=TEST_CONFIG=yes\n",
                    encoding="utf-8",
                )
                args = ["--nosystem_rc", "--nohome_rc", command, "--config=execution"]
                expected = ["--repo_env=COMMON=yes", "--remote_executor="]
                if inherits_test:
                    expected.append("--remote_executor=grpc://test-executor")
                expected.extend(
                    ["--repo_env=COMMAND=yes", "--config=execution", "--repo_env=BUILD_CONFIG=yes"]
                )
                if inherits_test:
                    expected.append("--repo_env=TEST_CONFIG=yes")

                self.assertEqual(
                    bazelrc._bazel_effective_options(args, env={}, repo_root=repo_root), expected
                )
                self.assertEqual(
                    bazelrc._remote_execution_disabled_by_args(args, env={}, repo_root=repo_root),
                    not inherits_test,
                )


if __name__ == "__main__":
    unittest.main()
