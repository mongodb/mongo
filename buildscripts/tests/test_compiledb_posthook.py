import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.append(".")

from bazel.wrapper_hook.plus_interface import test_runner_interface


def _noop_buildozer(*_args, **_kwargs):
    return ""


class CompiledbPosthookTest(unittest.TestCase):
    """Verify that the compiledb posthook is set up from both the 'compiledb' target
    and the '--config=compiledb' config flag."""

    PATCHES = {
        "generate": "bazel.wrapper_hook.plus_interface.generate_compiledb",
        "prepare": "bazel.wrapper_hook.plus_interface.prepare_compiledb_posthook_args",
        "clear": "bazel.wrapper_hook.plus_interface.clear_compiledb_posthook_state",
        "swap": "bazel.wrapper_hook.plus_interface.swap_default_config",
    }

    def _run(self, args, **kwargs):
        with (
            mock.patch(self.PATCHES["generate"]) as mock_gen,
            mock.patch(self.PATCHES["prepare"], return_value=["build", "--done"]) as mock_prep,
            mock.patch(self.PATCHES["clear"]),
            mock.patch(self.PATCHES["swap"], return_value=None),
            mock.patch.dict(os.environ, {"CI": "1"}, clear=False),
        ):
            result = test_runner_interface(
                args,
                autocomplete_query=False,
                get_buildozer_output=_noop_buildozer,
                enterprise=True,
                atlas=True,
                **kwargs,
            )
        return result, mock_gen, mock_prep

    # ------------------------------------------------------------------
    # compiledb TARGET triggers generate_compiledb (direct path)
    # ------------------------------------------------------------------

    def test_compiledb_target_calls_generate_compiledb(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", "compiledb"])
        mock_gen.assert_called_once()
        mock_prep.assert_not_called()
        self.assertEqual(result, [])

    def test_compiledb_target_with_colon_calls_generate_compiledb(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", ":compiledb"])
        mock_gen.assert_called_once()
        mock_prep.assert_not_called()

    def test_compiledb_target_full_label_calls_generate_compiledb(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", "//:compiledb"])
        mock_gen.assert_called_once()
        mock_prep.assert_not_called()

    # ------------------------------------------------------------------
    # --config=compiledb triggers prepare_compiledb_posthook_args
    # ------------------------------------------------------------------

    def test_config_compiledb_calls_prepare_posthook(self):
        result, mock_gen, mock_prep = self._run(
            ["bazel", "build", "--config=compiledb", "//src/mongo/..."]
        )
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()
        call_kwargs = mock_prep.call_args
        self.assertEqual(call_kwargs.kwargs["command"], "build")
        self.assertFalse(call_kwargs.kwargs["setup_clang_tidy"])

    def test_config_compiledb_aspect_calls_prepare_posthook(self):
        result, mock_gen, mock_prep = self._run(
            ["bazel", "build", "--config=compiledb-aspect", "//src/mongo/..."]
        )
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()

    def test_config_compiledb_with_startup_args_forwards_them(self):
        result, mock_gen, mock_prep = self._run(
            [
                "bazel",
                "--output_user_root=/tmp/cache",
                "build",
                "--config=compiledb",
                "//src/mongo/...",
            ]
        )
        mock_prep.assert_called_once()
        call_kwargs = mock_prep.call_args
        self.assertEqual(call_kwargs.kwargs["startup_args"], ["--output_user_root=/tmp/cache"])

    def test_config_compiledb_with_target_pattern_file(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write("//src/mongo/db:mongod\n//src/mongo/s:mongos\n")
            pattern_file = f.name
        try:
            result, mock_gen, mock_prep = self._run(
                [
                    "bazel",
                    "build",
                    "--config=compiledb",
                    f"--target_pattern_file={pattern_file}",
                ]
            )
            mock_prep.assert_called_once()
            call_kwargs = mock_prep.call_args
            self.assertEqual(call_kwargs.kwargs["build_targets"], [])
            posthook_targets = call_kwargs.kwargs["compiledb_targets"]
            self.assertIn("//src/mongo/db:mongod", posthook_targets)
            self.assertIn("//src/mongo/s:mongos", posthook_targets)
        finally:
            os.unlink(pattern_file)

    def test_config_compiledb_returns_prepare_result(self):
        result, _, _ = self._run(["bazel", "build", "--config=compiledb", "//src/mongo/..."])
        self.assertEqual(result, ["build", "--done"])

    # ------------------------------------------------------------------
    # Plain build (no compiledb) triggers neither
    # ------------------------------------------------------------------

    def test_plain_build_skips_compiledb(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", "//src/mongo/..."])
        mock_gen.assert_not_called()
        mock_prep.assert_not_called()
        self.assertEqual(result, ["build", "//src/mongo/..."])

    # ------------------------------------------------------------------
    # compiledb target takes precedence over --config=compiledb
    # ------------------------------------------------------------------

    def test_compiledb_target_takes_precedence_over_config(self):
        """When both 'compiledb' target and --config=compiledb appear,
        generate_compiledb is called (target path), not prepare_posthook."""
        result, mock_gen, mock_prep = self._run(
            ["bazel", "build", "--config=compiledb", "compiledb"]
        )
        mock_gen.assert_called_once()
        mock_prep.assert_not_called()

    # ------------------------------------------------------------------
    # Non-build commands with --config=compiledb don't trigger posthook
    # ------------------------------------------------------------------

    def test_config_compiledb_on_test_command_does_not_trigger_posthook(self):
        result, mock_gen, mock_prep = self._run(
            ["bazel", "test", "--config=compiledb", "//src/mongo/..."]
        )
        mock_gen.assert_not_called()
        mock_prep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
