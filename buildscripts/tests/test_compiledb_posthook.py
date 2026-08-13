import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.append(".")

from bazel.wrapper_hook import compiledb
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
    # compiledb TARGET uses the same integrated posthook path as --config=compiledb
    # ------------------------------------------------------------------

    def test_compiledb_target_calls_prepare_posthook(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", "compiledb"])
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()
        self.assertEqual(result, ["build", "--done"])
        self.assertEqual(mock_prep.call_args.kwargs["build_targets"], ["//src/..."])

    def test_compiledb_target_with_colon_calls_prepare_posthook(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", ":compiledb"])
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()

    def test_compiledb_target_preserves_copt_flag_value(self):
        result, mock_gen, mock_prep = self._run(
            [
                "bazel",
                "build",
                "compiledb",
                "--copt",
                "FOO",
                "--keep_going",
                "//src/mongo/base:error_codes",
            ]
        )

        mock_gen.assert_not_called()
        mock_prep.assert_called_once()
        self.assertEqual(
            mock_prep.call_args.kwargs["build_flags"],
            ["--copt", "FOO", "--keep_going"],
        )
        self.assertEqual(
            mock_prep.call_args.kwargs["build_targets"],
            ["//src/mongo/base:error_codes"],
        )

    def test_compiledb_target_preserves_interleaved_build_arguments(self):
        result, mock_gen, mock_prep = self._run(
            [
                "bazel",
                "build",
                "compiledb",
                "//src/mongo/base:error_codes",
                "-c",
                "dbg",
                "--features",
                "thin_lto",
            ]
        )

        mock_gen.assert_not_called()
        mock_prep.assert_called_once()
        call_kwargs = mock_prep.call_args.kwargs
        self.assertEqual(
            call_kwargs["build_flags"],
            ["-c", "dbg", "--features", "thin_lto"],
        )
        self.assertEqual(
            call_kwargs["build_args"],
            [
                "//src/mongo/base:error_codes",
                "-c",
                "dbg",
                "--features",
                "thin_lto",
            ],
        )

    def test_compiledb_target_full_label_calls_prepare_posthook(self):
        result, mock_gen, mock_prep = self._run(["bazel", "build", "//:compiledb"])
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()

    def test_compiledb_target_preserves_target_scope_environment(self):
        with mock.patch.dict(
            os.environ,
            {"MONGO_COMPILEDB_TARGET_SCOPE": "//src/mongo/db/..."},
            clear=False,
        ):
            result, mock_gen, mock_prep = self._run(["bazel", "build", "compiledb"])

        mock_gen.assert_not_called()
        mock_prep.assert_called_once()
        self.assertEqual(mock_prep.call_args.kwargs["build_targets"], ["//src/mongo/db/..."])
        self.assertEqual(mock_prep.call_args.kwargs["compiledb_targets"], [])

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

    def test_posthook_state_uses_invocation_specific_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = os.path.join(temp_dir, "posthook_state.json")
            buildevents_path = os.path.join(temp_dir, "buildevents.json")
            with mock.patch.dict(
                os.environ,
                {compiledb.COMPILEDB_POSTHOOK_STATE_ENV: state_path},
            ):
                compiledb.prepare_compiledb_posthook_args(
                    bazel_bin="bazel",
                    startup_args=[f"--output_base={temp_dir}"],
                    command="build",
                    build_flags=[f"--build_event_json_file={buildevents_path}"],
                    build_targets=["//src/..."],
                    persistent_compdb=False,
                    enterprise=True,
                    atlas=True,
                )
                self.assertTrue(os.path.exists(state_path))
                compiledb.clear_compiledb_posthook_state()
                self.assertFalse(os.path.exists(state_path))

    def test_posthook_state_preserves_empty_requested_targets(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = os.path.join(temp_dir, "posthook_state.json")
            buildevents_path = os.path.join(temp_dir, "buildevents.json")
            with mock.patch.dict(
                os.environ,
                {compiledb.COMPILEDB_POSTHOOK_STATE_ENV: state_path},
            ):
                compiledb.prepare_compiledb_posthook_args(
                    bazel_bin="bazel",
                    startup_args=[f"--output_base={temp_dir}"],
                    command="build",
                    build_flags=[f"--build_event_json_file={buildevents_path}"],
                    build_targets=["//src/..."],
                    persistent_compdb=False,
                    enterprise=True,
                    atlas=True,
                    compiledb_targets=[],
                )
                with open(state_path, encoding="utf-8") as state_file:
                    state = json.load(state_file)
                self.assertEqual(state["requested_targets"], [])
                compiledb.clear_compiledb_posthook_state()

    def test_prepare_preserves_interleaved_build_arguments(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = os.path.join(temp_dir, "posthook_state.json")
            original_args = [
                "//src/mongo/base:error_codes",
                "-c",
                "dbg",
                "--features",
                "thin_lto",
            ]
            with mock.patch.dict(
                os.environ,
                {compiledb.COMPILEDB_POSTHOOK_STATE_ENV: state_path},
            ):
                try:
                    result = compiledb.prepare_compiledb_posthook_args(
                        bazel_bin="bazel",
                        startup_args=[f"--output_base={temp_dir}"],
                        command="build",
                        build_flags=original_args[1:],
                        build_targets=[original_args[0]],
                        persistent_compdb=False,
                        enterprise=True,
                        atlas=True,
                        build_args=original_args,
                    )
                    self.assertEqual(
                        result[: 2 + len(original_args)],
                        [f"--output_base={temp_dir}", "build", *original_args],
                    )
                finally:
                    if os.path.exists(state_path):
                        with open(state_path, encoding="utf-8") as state_file:
                            buildevents_path = json.load(state_file)["buildevents_path"]
                        if os.path.exists(buildevents_path):
                            os.remove(buildevents_path)
                    compiledb.clear_compiledb_posthook_state()

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
        the target path uses the posthook, not the config-only path."""
        result, mock_gen, mock_prep = self._run(
            ["bazel", "build", "--config=compiledb", "compiledb"]
        )
        mock_gen.assert_not_called()
        mock_prep.assert_called_once()

    # ------------------------------------------------------------------
    # Non-build commands with --config=compiledb don't trigger posthook
    # ------------------------------------------------------------------

    def test_config_compiledb_on_test_command_does_not_trigger_posthook(self):
        result, mock_gen, mock_prep = self._run(
            ["bazel", "test", "--config=compiledb", "//src/mongo/..."]
        )
        mock_gen.assert_not_called()
        mock_prep.assert_not_called()


class CompiledbExternalRepoMaterializationTest(unittest.TestCase):
    def _create_external_repo(self, temp_dir):
        output_base = pathlib.Path(temp_dir)
        repo = output_base / "external" / "repo"
        repo.mkdir(parents=True)
        (repo / "header.h").write_text("header\n", encoding="utf-8")
        return output_base, repo

    def test_existing_junction_is_not_copied(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base, repo = self._create_external_repo(temp_dir)
            link = output_base / "execroot" / "_main" / "external" / repo.name
            link.mkdir(parents=True)

            with (
                mock.patch.object(compiledb, "_windows_symlinks_available", return_value=False),
                mock.patch.object(pathlib.Path, "is_junction", return_value=True),
                mock.patch.object(compiledb, "_paths_are_same", return_value=True),
                mock.patch.object(compiledb, "_copy_path") as copy_path,
            ):
                compiledb.materialize_execroot_external_symlinks(output_base)

            copy_path.assert_not_called()

    def test_existing_copy_only_copies_changed_files(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base, repo = self._create_external_repo(temp_dir)
            with mock.patch.object(compiledb, "_windows_symlinks_available", return_value=False):
                compiledb.materialize_execroot_external_symlinks(output_base)

                with mock.patch.object(
                    compiledb.shutil, "copy2", wraps=compiledb.shutil.copy2
                ) as copy2:
                    compiledb.materialize_execroot_external_symlinks(output_base)
                    copy2.assert_not_called()

                (repo / "header.h").write_text("updated header\n", encoding="utf-8")
                with mock.patch.object(
                    compiledb.shutil, "copy2", wraps=compiledb.shutil.copy2
                ) as copy2:
                    compiledb.materialize_execroot_external_symlinks(output_base)
                    copy2.assert_called_once()


if __name__ == "__main__":
    unittest.main()
