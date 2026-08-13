import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import compiledb


class CompiledbPosthookTest(unittest.TestCase):
    def test_clang_tidy_targets_are_built_with_compiledb(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = pathlib.Path(temp_dir)
            bazel_bin = temp_root / "bazel-bin"
            plugin_path = bazel_bin / "src" / "mongo" / "tools" / "mongo_tidy_checks"
            plugin_path.mkdir(parents=True)
            (bazel_bin / ".clang-tidy").touch()
            (plugin_path / "libmongo_tidy_checks.so").touch()

            with (
                mock.patch.object(compiledb, "REPO_ROOT", temp_root),
                mock.patch.object(compiledb, "write_wrapper_hook_bazelrc"),
                mock.patch.object(compiledb, "_run_build_command") as run_build,
                mock.patch.object(compiledb, "materialize_execroot_external_symlinks"),
                mock.patch.object(
                    compiledb,
                    "load_compile_command_fragments",
                    return_value=[
                        {
                            "file": "src/example.cc",
                            "arguments": ["clang++", "-c", "src/example.cc"],
                        }
                    ],
                ),
                mock.patch.object(compiledb, "write_compile_commands"),
                mock.patch.object(compiledb, "materialize_clang_tidy_ide_files"),
            ):
                compiledb._generate_compiledb_via_aspect(
                    bazel_bin="bazel",
                    persistent_compdb=False,
                    enterprise=True,
                    atlas=True,
                    requested_targets=["//src/..."],
                    setup_clang_tidy=True,
                    prepared_output_base=temp_root,
                    prepared_buildevents_path=str(temp_root / "events.json"),
                    delete_buildevents=False,
                )

            self.assertEqual(run_build.call_count, 2)
            self.assertEqual(
                run_build.call_args_list[0].args[0][-1:],
                ["//src/..."],
            )
            self.assertEqual(
                run_build.call_args_list[1].args[0][-3:],
                [
                    "//:setup_clang_tidy",
                    "//:clang_tidy_config",
                    "//src/mongo/tools/mongo_tidy_checks:mongo_tidy_checks",
                ],
            )

    def test_clang_tidy_build_failure_does_not_abort_compiledb(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = pathlib.Path(temp_dir)
            bazel_bin = temp_root / "bazel-bin"
            plugin_path = bazel_bin / "src" / "mongo" / "tools" / "mongo_tidy_checks"
            plugin_path.mkdir(parents=True)
            (bazel_bin / ".clang-tidy").touch()
            (plugin_path / "libmongo_tidy_checks.so").touch()

            with (
                mock.patch.object(compiledb, "REPO_ROOT", temp_root),
                mock.patch.object(compiledb, "write_wrapper_hook_bazelrc"),
                mock.patch.object(
                    compiledb,
                    "_run_build_command",
                    side_effect=[None, RuntimeError("clang-tidy build failed")],
                ) as run_build,
                mock.patch.object(compiledb, "materialize_execroot_external_symlinks"),
                mock.patch.object(
                    compiledb,
                    "load_compile_command_fragments",
                    return_value=[
                        {
                            "file": "src/example.cc",
                            "arguments": ["clang++", "-c", "src/example.cc"],
                        }
                    ],
                ),
                mock.patch.object(compiledb, "write_compile_commands"),
                mock.patch.object(
                    compiledb, "materialize_clang_tidy_ide_files"
                ) as materialize_tidy,
            ):
                compiledb._generate_compiledb_via_aspect(
                    bazel_bin="bazel",
                    persistent_compdb=False,
                    enterprise=True,
                    atlas=True,
                    requested_targets=["//src/..."],
                    setup_clang_tidy=True,
                    prepared_output_base=temp_root,
                    prepared_buildevents_path=str(temp_root / "events.json"),
                    delete_buildevents=False,
                )

            self.assertEqual(run_build.call_count, 2)
            materialize_tidy.assert_not_called()

    def test_copy_if_changed_replaces_same_metadata_different_contents(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = pathlib.Path(temp_dir)
            source_path = temp_root / "source.h"
            destination_path = temp_root / "destination.h"
            source_path.write_bytes(b"new")
            destination_path.write_bytes(b"old")

            same_mtime_ns = 1_000_000_000
            os.utime(source_path, ns=(same_mtime_ns, same_mtime_ns))
            os.utime(destination_path, ns=(same_mtime_ns, same_mtime_ns))

            compiledb._copy_if_changed(source_path, destination_path)

            self.assertEqual(destination_path.read_bytes(), b"new")

    def test_missing_build_event_file_is_ignored(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = pathlib.Path(temp_dir) / "posthook_state.json"
            missing_events_path = pathlib.Path(temp_dir) / "missing-events.json"
            state_path.write_text(
                json.dumps(
                    {
                        "start_time": 0,
                        "buildevents_path": str(missing_events_path),
                    }
                ),
                encoding="utf-8",
            )

            with mock.patch.dict(
                os.environ,
                {compiledb.COMPILEDB_POSTHOOK_STATE_ENV: str(state_path)},
                clear=False,
            ):
                with mock.patch.object(compiledb, "_generate_compiledb_via_aspect") as generate:
                    compiledb.finalize_compiledb_posthook("bazel", enterprise=False, atlas=False)

            generate.assert_not_called()
            self.assertFalse(state_path.exists())


if __name__ == "__main__":
    unittest.main()
