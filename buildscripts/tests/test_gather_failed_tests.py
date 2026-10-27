"""Unit tests for buildscripts/gather_failed_tests.py."""

import contextlib
import gzip
import json
import os
import shlex
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from xml.etree import ElementTree
from zipfile import ZipFile

from buildscripts import create_bazel_test_report
from buildscripts import gather_failed_tests as under_test


class TestGatherFailedTests(unittest.TestCase):
    def test_missing_build_events_file_skips_failed_test_gathering(self):
        original_cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                os.chdir(tmpdir)
                with patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}):
                    with patch("builtins.print") as mock_print:
                        under_test.main("missing_build_events.json")
            finally:
                os.chdir(original_cwd)

            self.assertFalse(Path(tmpdir, "dist-tests").exists())
            mock_print.assert_called_once()
            self.assertIn("missing_build_events.json", mock_print.call_args.args[0])


class TestRelinkBinariesWithSymbols(unittest.TestCase):
    def _relink(self, tmpdir, build_flags):
        Path(tmpdir, ".bazel_build_flags").write_text(build_flags, encoding="utf-8")
        original_cwd = os.getcwd()
        try:
            os.chdir(tmpdir)
            with (
                patch("buildscripts.gather_failed_tests.subprocess.run") as mock_run,
                patch("builtins.print"),
            ):
                under_test._relink_binaries_with_symbols(["//src/mongo/db/exec/sbe:db_sbe_test"])
        finally:
            os.chdir(original_cwd)
        command = mock_run.call_args.args[0]
        repro = Path(tmpdir, ".failed_unittest_repro.txt").read_text(encoding="utf-8")
        return command, repro

    def test_preserves_original_configuration_and_downloads_symbols(self):
        for config_flags in (
            "--config=remote_link",
            "--config=windows_unittest",
            "--config windows_unittest",
            "--config=remote_unittest",
        ):
            with self.subTest(config_flags=config_flags), tempfile.TemporaryDirectory() as tmpdir:
                saved_flags = shlex.split(config_flags + " --remote_download_outputs=minimal")
                command, repro = self._relink(tmpdir, shlex.join(saved_flags))
                expected_flags = [*saved_flags, "--remote_download_outputs=toplevel"]
                self.assertEqual(
                    command,
                    [
                        under_test._bazel_binary(),
                        "build",
                        *expected_flags,
                        "//src/mongo/db/exec/sbe:db_sbe_test",
                    ],
                )
                self.assertEqual(
                    shlex.split(repro),
                    [
                        under_test._bazel_binary(),
                        "test",
                        *expected_flags,
                        "//src/mongo/db/exec/sbe:db_sbe_test",
                    ],
                )

    def test_repro_preserves_quoted_arguments(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            flags = [
                "--action_env=EXAMPLE=value with spaces",
                "--remote_download_regex=.*\\.(exe|pdb)$",
            ]
            command, repro = self._relink(tmpdir, shlex.join(flags))
            self.assertEqual(shlex.split(repro)[2:], command[2:])


class TestCopyBinsToUpload(unittest.TestCase):
    def _run_in(self, tmpdir):
        upload_bin_dir = Path(tmpdir, "upload", "bin")
        upload_lib_dir = Path(tmpdir, "upload", "lib")
        upload_bin_dir.mkdir(parents=True)
        upload_lib_dir.mkdir(parents=True)
        original_cwd = os.getcwd()
        try:
            os.chdir(tmpdir)
            under_test._copy_bins_to_upload(upload_bin_dir, upload_lib_dir)
        finally:
            os.chdir(original_cwd)
        return upload_bin_dir, upload_lib_dir

    def test_collects_from_transitioned_config_without_bazel_bin_symlink(self):
        # A suite whose shard count comes from a rule transition is built in its own
        # configuration, so the relink leaves bazel-bin pointing elsewhere (or absent) and the
        # binaries are only reachable under bazel-out/<config>/bin/src.
        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir, "bazel-out", "k8-opt-ST-abc123", "bin", "src", "mongo")
            src.mkdir(parents=True)
            (src / "mongod.debug").touch()
            (src / "libfoo.so").touch()

            upload_bin_dir, upload_lib_dir = self._run_in(tmpdir)

            self.assertTrue((upload_bin_dir / "mongod.debug").exists())
            self.assertTrue((upload_lib_dir / "libfoo.so").exists())

    def test_collects_windows_minidumps_from_runfiles_trees(self):
        # The mongo exception filter writes .mdmp minidumps next to the test binary inside its
        # runfiles tree (which lives under bazel-out/<config>/bin/src); they must be uploaded
        # alongside the .exe and .pdb they pair with.
        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir, "bazel-out", "x64_windows-opt", "bin", "src", "mongo", "db")
            src.mkdir(parents=True)
            (src / "db_ftdc_test.exe").touch()
            (src / "db_ftdc_test.pdb").touch()
            runfiles = Path(
                src,
                "db_ftdc_test.exe.runfiles",
                "_main",
                "src",
                "mongo",
                "db",
            )
            runfiles.mkdir(parents=True)
            (runfiles / "db_ftdc_test.2026-09-19T17-21-34.mdmp").touch()

            upload_bin_dir, _ = self._run_in(tmpdir)

            self.assertTrue((upload_bin_dir / "db_ftdc_test.exe").exists())
            self.assertTrue((upload_bin_dir / "db_ftdc_test.pdb").exists())
            self.assertTrue((upload_bin_dir / "db_ftdc_test.2026-09-19T17-21-34.mdmp").exists())

    def test_does_not_copy_twice_when_symlink_and_glob_overlap(self):
        # bazel-bin normally resolves into one of the globbed directories; the dedup by resolved
        # path keeps that from being walked twice.
        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir, "bazel-out", "k8-opt", "bin", "src", "mongo")
            src.mkdir(parents=True)
            (src / "mongod.debug").touch()
            Path(tmpdir, "bazel-bin").symlink_to(Path(tmpdir, "bazel-out", "k8-opt", "bin"))

            upload_bin_dir, _ = self._run_in(tmpdir)

            self.assertEqual([p.name for p in upload_bin_dir.iterdir()], ["mongod.debug"])

    def test_no_bin_directories_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            upload_bin_dir, upload_lib_dir = self._run_in(tmpdir)
            self.assertEqual(list(upload_bin_dir.iterdir()), [])
            self.assertEqual(list(upload_lib_dir.iterdir()), [])


class TestRerunFailedTests(unittest.TestCase):
    def test_reruns_failed_tests_with_relink_flags(self):
        # The diagnostic run reuses the saved build configuration and downloaded symbols.
        with (
            patch("buildscripts.gather_failed_tests.subprocess.run") as mock_run,
            patch("builtins.print"),
        ):
            under_test._rerun_failed_tests(
                ["//src/mongo/logv2:logv2_test", "//src/mongo/db/ftdc:db_ftdc_test"],
                ["--keep_going", "--remote_download_outputs=toplevel"],
            )

        command = mock_run.call_args.args[0]
        self.assertEqual(command[0], under_test._bazel_binary())
        self.assertEqual(command[1], "test")
        self.assertEqual(
            command[2:],
            [
                "--keep_going",
                "--remote_download_outputs=toplevel",
                "--nocache_test_results",
                "--nozip_undeclared_test_outputs",
                "--test_strategy=standalone",
                "--strategy=TestRunner=standalone",
                "//src/mongo/logv2:logv2_test",
                "//src/mongo/db/ftdc:db_ftdc_test",
            ],
        )

    def test_rerun_overrides_saved_cache_test_results(self):
        with (
            patch("buildscripts.gather_failed_tests.subprocess.run") as mock_run,
            patch("builtins.print"),
        ):
            mock_run.return_value.returncode = 0
            under_test._rerun_failed_tests(
                ["//src/mongo/logv2:logv2_test"], ["--cache_test_results=yes"]
            )
        command = mock_run.call_args.args[0]
        self.assertGreater(
            command.index("--nocache_test_results"), command.index("--cache_test_results=yes")
        )
        self.assertLess(
            command.index("--nocache_test_results"), command.index("//src/mongo/logv2:logv2_test")
        )

    def test_rerun_exposes_dumps_when_saved_flags_zip_outputs(self):
        with patch.object(under_test.subprocess, "run") as mock_run:
            mock_run.return_value.returncode = 0
            under_test._rerun_failed_tests(
                ["//src/mongo/logv2:logv2_test"], ["--zip_undeclared_test_outputs"]
            )
        command = mock_run.call_args.args[0]
        self.assertGreater(
            command.index("--nozip_undeclared_test_outputs"),
            command.index("--zip_undeclared_test_outputs"),
        )

    def test_rerun_failure_is_not_fatal(self):
        # The task has already failed when the rerun happens, and test failures during the
        # rerun are expected; the rerun is purely diagnostic and must not raise.
        with (
            patch("buildscripts.gather_failed_tests.subprocess.run") as mock_run,
            patch("builtins.print"),
        ):
            mock_run.return_value.returncode = 1
            under_test._rerun_failed_tests(["//src/mongo/logv2:logv2_test"], [])

    def test_rerun_overrides_remote_test_execution(self):
        with patch.object(under_test.subprocess, "run") as mock_run:
            under_test._rerun_failed_tests(
                ["//src/mongo/logv2:logv2_test"],
                ["--test_strategy=remote", "--strategy=TestRunner=remote"],
            )
        command = mock_run.call_args.args[0]
        self.assertGreater(
            command.index("--test_strategy=standalone"), command.index("--test_strategy=remote")
        )
        self.assertGreater(
            command.index("--strategy=TestRunner=standalone"),
            command.index("--strategy=TestRunner=remote"),
        )


class TestSnapshotTestLogs(unittest.TestCase):
    def _testlogs_dir(self, tmpdir, label):
        testlogs_dir = under_test._testlogs_dir_for_label(label)
        full_dir = Path(tmpdir, testlogs_dir)
        full_dir.mkdir(parents=True, exist_ok=True)
        return full_dir

    @contextlib.contextmanager
    def _in_tmpdir(self, tmpdir):
        original_cwd = os.getcwd()
        try:
            os.chdir(tmpdir)
            yield
        finally:
            os.chdir(original_cwd)

    def test_snapshot_restores_original_logs_over_rerun_logs(self):
        # The create_bazel_test_report teardown step reads bazel-testlogs after the rerun;
        # restoring the snapshot keeps the report faithful to the original run.
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                (testlogs_dir / "test.xml").write_text("original xml", encoding="utf-8")
                (testlogs_dir / "test.log").write_bytes(b"original log")

                snapshot = under_test._snapshot_test_logs(under_test._find_testlogs_dirs([label]))

                (testlogs_dir / "test.xml").write_text("rerun xml", encoding="utf-8")
                (testlogs_dir / "test.log").write_bytes(b"rerun log")
                # Bazel marks the testlogs entries read-only; the restore must clear it
                # (on Windows the read-only attribute makes write_bytes fail).
                (testlogs_dir / "test.xml").chmod(0o444)
                (testlogs_dir / "test.log").chmod(0o444)

                under_test._restore_test_logs(snapshot)

                self.assertEqual(
                    (testlogs_dir / "test.xml").read_text(encoding="utf-8"), "original xml"
                )
                self.assertEqual((testlogs_dir / "test.log").read_bytes(), b"original log")

    def test_restore_failure_is_not_fatal(self):
        # The upload matters more than the restore; a failed restore must not crash the
        # gather (7481 lost its whole binary upload to a PermissionError on Windows).
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                (testlogs_dir / "test.xml").write_text("original xml", encoding="utf-8")

                snapshot = under_test._snapshot_test_logs(under_test._find_testlogs_dirs([label]))
                # Point a second entry at a path whose parent is a FILE so its restore
                # fails with NotADirectoryError/PermissionError.
                Path(tmpdir, "blocking-file").write_text("", encoding="utf-8")
                bad_entry = {Path(tmpdir, "blocking-file", "test.xml"): b"contents"}

                under_test._restore_test_logs({**snapshot, **bad_entry})

                # The original snapshot entry was restored despite the bad one failing.
                self.assertEqual(
                    (testlogs_dir / "test.xml").read_text(encoding="utf-8"), "original xml"
                )

    def test_snapshot_restores_nested_logs_and_removes_new_rerun_logs(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                original = {}
                for directory in ["shard_1_of_2", "run_1", "run_2/shard_2_of_2/attempt_1"]:
                    log_dir = testlogs_dir / directory
                    log_dir.mkdir(parents=True)
                    for filename in ["test.xml", "test.log"]:
                        path = log_dir / filename
                        if filename == "test.xml":
                            contents = (
                                '<testsuites><testsuite><testcase name="original" time="1">'
                                '<error message="original failure"/></testcase></testsuite>'
                                "</testsuites>"
                            ).encode()
                        else:
                            contents = f"original failure: {directory}/{filename}".encode()
                        path.write_bytes(contents)
                        original[path] = contents

                snapshot = under_test._snapshot_test_logs(under_test._find_testlogs_dirs([label]))
                passing_xml = (
                    '<testsuites><testsuite><testcase name="rerun" time="1"/>'
                    "</testsuite></testsuites>"
                ).encode()
                for path in original:
                    path.write_bytes(passing_xml if path.suffix == ".xml" else b"passing rerun")
                    path.chmod(0o444)

                added_dir = testlogs_dir / "run_3"
                added_dir.mkdir()
                for filename in ["test.xml", "test.log"]:
                    (added_dir / filename).write_bytes(
                        passing_xml if filename == "test.xml" else b"new passing rerun"
                    )
                    (added_dir / filename).chmod(0o444)
                dump = added_dir / "diagnostic.mdmp"
                dump.touch()
                other_target = self._testlogs_dir(tmpdir, "//src/mongo/logv2:other_test")
                (other_target / "test.xml").write_bytes(passing_xml)
                (other_target / "test.log").write_bytes(b"unrelated passing result")

                under_test._restore_test_logs(snapshot, under_test._find_testlogs_dirs([label]))

                self.assertEqual(
                    {path.resolve(): value for path, value in snapshot.items()},
                    {path.resolve(): value for path, value in original.items()},
                )
                for path, contents in original.items():
                    self.assertEqual(path.read_bytes(), contents)
                self.assertFalse((added_dir / "test.xml").exists())
                self.assertFalse((added_dir / "test.log").exists())
                self.assertTrue(dump.exists())
                self.assertEqual((other_target / "test.xml").read_bytes(), passing_xml)

                # The report discovers every nested test.xml. It must see exactly
                # the three original failures and the unrelated passing target.
                testcases = [
                    ElementTree.parse(path).find("testsuite/testcase")
                    for path in Path("bazel-testlogs").rglob("test.xml")
                ]
                self.assertCountEqual(
                    ["fail" if case.find("error") is not None else "pass" for case in testcases],
                    ["fail", "fail", "fail", "pass"],
                )

    def test_restore_recreates_logs_removed_by_rerun(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                log_path = testlogs_dir / "test.xml"
                log_path.write_bytes(b"original failure")
                snapshot = under_test._snapshot_test_logs(under_test._find_testlogs_dirs([label]))
                log_path.unlink()

                under_test._restore_test_logs(snapshot)

                self.assertEqual(log_path.read_bytes(), b"original failure")

    def test_restore_removes_rerun_results_when_original_has_no_logs(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                snapshot = under_test._snapshot_test_logs(under_test._find_testlogs_dirs([label]))
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                log_path = testlogs_dir / "test.xml"
                log_path.write_bytes(b"new diagnostic result")

                under_test._restore_test_logs(snapshot, under_test._find_testlogs_dirs([label]))

                self.assertFalse(log_path.exists())

    def test_main_restores_logs_when_diagnostic_rerun_raises(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                testlogs_dir = self._testlogs_dir(tmpdir, label)
                log_path = testlogs_dir / "test.xml"
                log_path.write_bytes(b"original failure")
                Path("build_events.json").touch()

                def interrupted_rerun(*args):
                    log_path.write_bytes(b"diagnostic rerun")
                    raise RuntimeError("diagnostic rerun interrupted")

                with (
                    patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                    patch.object(under_test, "process_bep", return_value=([label], [])),
                    patch.object(under_test, "_relink_binaries_with_symbols", return_value=[]),
                    patch.object(under_test, "_rerun_failed_tests", side_effect=interrupted_rerun),
                    self.assertRaisesRegex(RuntimeError, "diagnostic rerun interrupted"),
                ):
                    under_test.main()

                self.assertEqual(log_path.read_bytes(), b"original failure")

    def test_snapshot_skips_missing_test_logs(self):
        # Tests that failed to build have no bazel-testlogs entry; the snapshot must tolerate
        # that and the restore must be a no-op.
        with tempfile.TemporaryDirectory() as tmpdir:
            with self._in_tmpdir(tmpdir):
                snapshot = under_test._snapshot_test_logs(
                    under_test._find_testlogs_dirs(["//src/mongo/db/never_built:never_test"])
                )

                self.assertEqual(snapshot, {})
                under_test._restore_test_logs(snapshot)

    def test_main_preserves_transitioned_logs_and_cleans_new_configurations(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir, contextlib.chdir(tmpdir):
            target = Path("src/mongo/logv2/logv2_test")
            original_dir = Path("bazel-out/k8-opt-ST-original/testlogs", target)
            original_dir.mkdir(parents=True)
            log_path = original_dir / "shard_1_of_2/test.xml"
            log_path.parent.mkdir()
            log_path.write_bytes(b"original failure")
            Path("build_events.json").touch()

            def rerun(*args):
                log_path.write_bytes(b"passing rerun")
                for directory in [original_dir / "run_2", Path("bazel-out/new/testlogs", target)]:
                    directory.mkdir(parents=True)
                    (directory / "test.xml").write_bytes(b"new passing result")
                    (directory / "test.log").write_bytes(b"passing rerun log")
                    (directory / "diagnostic.mdmp").touch()

            with (
                patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                patch.object(under_test, "process_bep", return_value=([label], [])),
                patch.object(under_test, "_relink_binaries_with_symbols", return_value=[]),
                patch.object(under_test, "_rerun_failed_tests", side_effect=rerun),
                patch.object(under_test, "_copy_bins_to_upload"),
            ):
                under_test.main()

            self.assertEqual(log_path.read_bytes(), b"original failure")
            self.assertEqual(list(Path("bazel-out").rglob("test.xml")), [log_path])
            self.assertEqual(list(Path("bazel-out").rglob("test.log")), [])
            self.assertEqual(len(list(Path("bazel-out").rglob("*.mdmp"))), 2)

    def test_main_retains_paths_when_relink_changes_testlogs_symlink(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir, contextlib.chdir(tmpdir):
            # Retain the resolved path even if it is no longer discoverable after relinking.
            original_root = Path("original-configuration/testlogs")
            testlogs_dir = original_root / "src/mongo/logv2/logv2_test"
            testlogs_dir.mkdir(parents=True)
            original_xml = (
                '<testsuites><testsuite><testcase name="original" time="1">'
                '<error message="original failure"/></testcase></testsuite></testsuites>'
            ).encode()
            (testlogs_dir / "test.xml").write_bytes(original_xml)
            (testlogs_dir / "test.log").write_bytes(b"original failure log")
            passing_dir = original_root / "src/mongo/logv2/passing_test"
            passing_dir.mkdir()
            (passing_dir / "test.xml").write_bytes(
                b'<testsuites><testsuite><testcase name="passing" time="1"/></testsuite></testsuites>'
            )
            (passing_dir / "test.log").write_bytes(b"original passing log")
            Path("bazel-testlogs").symlink_to(original_root.resolve(), target_is_directory=True)
            Path("build_events.json").touch()

            def relink(*args):
                new_root = Path("bazel-out/new/testlogs")
                new_root.mkdir(parents=True)
                Path("bazel-testlogs").unlink()
                Path("bazel-testlogs").symlink_to(new_root.resolve(), target_is_directory=True)
                return []

            def rerun(*args):
                (testlogs_dir / "test.xml").write_bytes(b"passing rerun")
                (testlogs_dir / "test.log").write_bytes(b"passing rerun log")
                (testlogs_dir / "run_2").mkdir()
                (testlogs_dir / "run_2/test.log").write_bytes(b"new rerun log")

            with (
                patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                patch.object(under_test, "process_bep", return_value=([label], [])),
                patch.object(under_test, "_relink_binaries_with_symbols", side_effect=relink),
                patch.object(under_test, "_rerun_failed_tests", side_effect=rerun),
                patch.object(under_test, "_copy_bins_to_upload"),
            ):
                under_test.main()

            self.assertEqual((testlogs_dir / "test.xml").read_bytes(), original_xml)
            self.assertFalse((testlogs_dir / "run_2/test.log").exists())
            with patch.object(create_bazel_test_report, "get_expansion", return_value=True):
                create_bazel_test_report.main("bazel-testlogs/")
            report = json.loads(Path("report.json").read_text())
            self.assertCountEqual(
                [
                    (result["test_file"], result["status"], result["log_raw"])
                    for result in report["results"]
                ],
                [
                    ("original", "fail", "original failure log"),
                    ("passing", "pass", "original passing log"),
                ],
            )

    def test_testlogs_dir_for_label_mapping(self):
        self.assertEqual(
            under_test._testlogs_dir_for_label("//src/mongo/logv2:logv2_test"),
            Path("bazel-testlogs/src/mongo/logv2/logv2_test"),
        )
        self.assertEqual(
            under_test._testlogs_dir_for_label("//:root_target"),
            Path("bazel-testlogs/root_target"),
        )


class TestPreserveTestOutputs(unittest.TestCase):
    def test_crash_handler_dumps_survive_relink_and_passing_or_crashing_rerun(self):
        label = "//src/mongo/logv2:logv2_test"
        dump_name = "logv2_test.2026-10-23T12-34-56.mdmp"
        for in_runfiles in [False, True]:
            for rerun_crashes in [False, True]:
                with (
                    self.subTest(in_runfiles=in_runfiles, rerun_crashes=rerun_crashes),
                    tempfile.TemporaryDirectory() as tmpdir,
                    contextlib.chdir(tmpdir),
                ):
                    # This configuration is reachable only through the convenience link,
                    # which relinking redirects before replacing the original output tree.
                    original_root = Path("original-configuration/bin")
                    binary_dir = original_root / "src/mongo/logv2"
                    binary_dir.mkdir(parents=True)
                    Path("bazel-bin").symlink_to(original_root.resolve(), target_is_directory=True)
                    dump_dir = binary_dir
                    if in_runfiles:
                        dump_dir /= "logv2_test.exe.runfiles/_main/src/mongo/logv2"
                        dump_dir.mkdir(parents=True)
                    original_dump = dump_dir / dump_name
                    original_dump.write_bytes(b"initial crash handler minidump")
                    original_dump.chmod(0o444)
                    Path("build_events.json").touch()

                    new_root = Path("bazel-out/windows-rerun/bin")
                    new_binary_dir = new_root / "src/mongo/logv2"

                    def relink(*args):
                        original_dump.chmod(0o644)
                        shutil.rmtree(original_root)
                        new_binary_dir.mkdir(parents=True)
                        (new_binary_dir / "logv2_test.exe").write_bytes(b"test binary")
                        (new_binary_dir / "logv2_test.pdb").write_bytes(b"debug symbols")
                        Path("bazel-bin").unlink()
                        Path("bazel-bin").symlink_to(new_root.resolve(), target_is_directory=True)
                        return []

                    def rerun(*args):
                        if rerun_crashes:
                            # The collector flattens paths, so preserve the first dump
                            # under a distinct name even if the second has the same name.
                            (new_binary_dir / dump_name).write_bytes(b"diagnostic minidump")

                    with (
                        patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                        patch.object(under_test, "process_bep", return_value=([label], [])),
                        patch.object(
                            under_test, "_relink_binaries_with_symbols", side_effect=relink
                        ),
                        patch.object(under_test, "_rerun_failed_tests", side_effect=rerun),
                    ):
                        under_test.main()

                    archived_dumps = list(Path("failed-test-outputs").rglob("*.mdmp"))
                    self.assertEqual(
                        [dump.read_bytes() for dump in archived_dumps],
                        [b"initial crash handler minidump"],
                    )
                    self.assertNotEqual(archived_dumps[0].name, dump_name)
                    self.assertTrue(archived_dumps[0].name.endswith(dump_name))
                    self.assertEqual(
                        Path("dist-tests/bin/logv2_test.pdb").read_bytes(), b"debug symbols"
                    )
                    staged_dumps = list(Path("dist-tests/bin").glob("*.mdmp"))
                    self.assertEqual(
                        [dump.read_bytes() for dump in staged_dumps],
                        [b"diagnostic minidump"] if rerun_crashes else [],
                    )

    def test_crash_handler_dumps_from_multiple_configurations_have_distinct_names(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir, contextlib.chdir(tmpdir):
            for config in ["windows-default", "windows-transitioned"]:
                binary_dir = Path("bazel-out", config, "bin/src/mongo/logv2")
                binary_dir.mkdir(parents=True)
                (binary_dir / "logv2_test.same-time.mdmp").write_bytes(config.encode())
            Path("bazel-bin").symlink_to(
                Path("bazel-out/windows-default/bin").resolve(), target_is_directory=True
            )
            Path("build_events.json").touch()

            with (
                patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                patch.object(under_test, "process_bep", return_value=([label], [])),
                patch.object(under_test, "_relink_binaries_with_symbols", return_value=[]),
                patch.object(under_test, "_rerun_failed_tests"),
            ):
                under_test.main()

            archived_dumps = list(Path("failed-test-outputs").rglob("*.mdmp"))
            self.assertCountEqual(
                [dump.read_bytes() for dump in archived_dumps],
                [b"windows-default", b"windows-transitioned"],
            )
            self.assertEqual(len({dump.name for dump in archived_dumps}), 2)

    def test_crash_handler_dump_backup_failure_preserves_outputs_without_running_bazel(self):
        label = "//src/mongo/logv2:logv2_test"
        with tempfile.TemporaryDirectory() as tmpdir, contextlib.chdir(tmpdir):
            binary_dir = Path("bazel-out/windows-original/bin/src/mongo/logv2")
            binary_dir.mkdir(parents=True)
            dump = binary_dir / "logv2_test.same-time.mdmp"
            dump.write_bytes(b"initial crash handler minidump")
            (binary_dir / "logv2_test.exe").write_bytes(b"original test binary")
            Path("build_events.json").touch()

            with (
                patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                patch.object(under_test, "process_bep", return_value=([label], [])),
                patch.object(shutil, "copy2", side_effect=PermissionError("backup unavailable")),
                patch.object(under_test, "_relink_binaries_with_symbols") as mock_relink,
                patch.object(under_test, "_rerun_failed_tests") as mock_rerun,
            ):
                under_test.main()

            mock_relink.assert_not_called()
            mock_rerun.assert_not_called()
            self.assertEqual(dump.read_bytes(), b"initial crash handler minidump")
            self.assertEqual(
                Path("dist-tests/bin/logv2_test.same-time.mdmp").read_bytes(),
                b"initial crash handler minidump",
            )
            self.assertEqual(
                Path("dist-tests/bin/logv2_test.exe").read_bytes(), b"original test binary"
            )

    def test_zipped_original_outputs_survive_a_passing_rerun(self):
        label = "//src/mongo/logv2:logv2_test"
        for zip_location in ["test.outputs/outputs.zip", "outputs.zip"]:
            with (
                self.subTest(zip_location=zip_location),
                tempfile.TemporaryDirectory() as tmpdir,
                contextlib.chdir(tmpdir),
            ):
                target = Path("bazel-out/config/testlogs/src/mongo/logv2/logv2_test")
                zip_path = target / zip_location
                zip_path.parent.mkdir(parents=True)
                original_outputs = {
                    "nested/dump_test.123.mdmp": b"original minidump",
                    "dump_test.123.core.gz": gzip.compress(b"original core"),
                    "boring_core_dumps.txt": b"123\n",
                    "other-output.txt": b"original output",
                }
                with ZipFile(zip_path, "w") as archive:
                    for name, contents in original_outputs.items():
                        archive.writestr(name, contents)
                Path("build_events.json").touch()

                def rerun(*args):
                    shutil.rmtree(target)
                    target.mkdir()
                    with ZipFile(target / "outputs.zip", "w"):
                        pass

                with (
                    patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                    patch.object(under_test, "process_bep", return_value=([label], [])),
                    patch.object(
                        under_test,
                        "_relink_binaries_with_symbols",
                        return_value=["--zip_undeclared_test_outputs"],
                    ),
                    patch.object(
                        under_test, "_rerun_failed_tests", side_effect=rerun
                    ) as mock_rerun,
                    patch.object(under_test, "_copy_bins_to_upload"),
                ):
                    under_test.main()

                mock_rerun.assert_called_once()
                dumps = list(Path(".").rglob("*.mdmp"))
                cores = list(Path(".").rglob("*.core.gz"))
                self.assertEqual([dump.read_bytes() for dump in dumps], [b"original minidump"])
                self.assertEqual(
                    [gzip.decompress(core.read_bytes()) for core in cores], [b"original core"]
                )
                self.assertNotEqual(dumps[0].name, "dump_test.123.mdmp")
                self.assertEqual(
                    [path.read_bytes() for path in Path(".").rglob("boring_core_dumps.txt")],
                    [b"123\n"],
                )
                self.assertEqual(
                    [path.read_bytes() for path in Path(".").rglob("other-output.txt")],
                    [b"original output"],
                )

    def test_backup_failure_skips_rerun_and_stages_binaries(self):
        label = "//src/mongo/logv2:logv2_test"
        for failure in ["dangling_symlink", "copy_error", "bad_zip"]:
            with (
                self.subTest(failure=failure),
                tempfile.TemporaryDirectory() as tmpdir,
                contextlib.chdir(tmpdir),
            ):
                outputs = Path("bazel-testlogs/src/mongo/logv2/logv2_test/test.outputs")
                outputs.mkdir(parents=True)
                dump = outputs / "dump_test.123.mdmp"
                dump.write_bytes(b"original dump")
                if failure == "dangling_symlink":
                    (outputs / "broken-output").symlink_to("missing-file")
                elif failure == "bad_zip":
                    (outputs / "outputs.zip").write_bytes(b"invalid zip")
                binary = Path("bazel-bin/src/mongo/logv2/logv2_test.debug")
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b"debug symbols")
                Path("build_events.json").touch()

                with (
                    patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                    patch.object(under_test, "process_bep", return_value=([label], [])),
                    patch.object(
                        under_test, "_relink_binaries_with_symbols", return_value=[]
                    ) as mock_relink,
                    patch.object(under_test, "_rerun_failed_tests") as mock_rerun,
                    patch.object(
                        shutil, "copytree", side_effect=PermissionError("output unavailable")
                    )
                    if failure == "copy_error"
                    else contextlib.nullcontext(),
                ):
                    under_test.main()

                mock_relink.assert_called_once()
                mock_rerun.assert_not_called()
                self.assertEqual(
                    Path("dist-tests/bin/logv2_test.debug").read_bytes(), b"debug symbols"
                )
                self.assertEqual(dump.read_bytes(), b"original dump")

    def test_original_outputs_survive_passing_or_crashing_rerun(self):
        label = "//src/mongo/logv2:logv2_test"
        for rerun_crashes in [False, True]:
            with (
                self.subTest(rerun_crashes=rerun_crashes),
                tempfile.TemporaryDirectory() as tmpdir,
                contextlib.chdir(tmpdir),
            ):
                testlogs_root = Path("bazel-out/k8-opt-ST-original/testlogs")
                target_dir = testlogs_root / "src/mongo/logv2/logv2_test"
                Path("bazel-testlogs").symlink_to(testlogs_root.resolve(), target_is_directory=True)
                outputs_dirs = [target_dir / f"shard_{shard}_of_2/test.outputs" for shard in [1, 2]]
                for shard, outputs_dir in enumerate(outputs_dirs, start=1):
                    outputs_dir.mkdir(parents=True)
                    (outputs_dir / "dump_test.123.mdmp").write_bytes(
                        f"original dump {shard}".encode()
                    )
                    (outputs_dir / "dump_test.123.core.gz").write_bytes(
                        gzip.compress(f"original core {shard}".encode())
                    )
                    (outputs_dir / "dump_test.123.core").write_bytes(
                        f"original plain core {shard}".encode()
                    )
                    (outputs_dir / "boring_core_dumps.txt").write_bytes(b"123\n")
                    (outputs_dir / "other-output.txt").write_bytes(b"original output")
                    for output in outputs_dir.iterdir():
                        output.chmod(0o444)
                Path("build_events.json").touch()

                def rerun(*args):
                    # Bazel removes undeclared outputs even when the rerun passes.
                    for outputs_dir in outputs_dirs:
                        for output in outputs_dir.iterdir():
                            output.chmod(0o644)
                        shutil.rmtree(outputs_dir)
                        outputs_dir.mkdir()
                    if rerun_crashes:
                        (outputs_dirs[0] / "dump_test.123.mdmp").write_bytes(b"rerun dump")
                        (outputs_dirs[0] / "dump_test.123.core.gz").write_bytes(
                            gzip.compress(b"rerun core")
                        )

                with (
                    patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}),
                    patch.object(under_test, "process_bep", return_value=([label], [])),
                    patch.object(under_test, "_relink_binaries_with_symbols", return_value=[]),
                    patch.object(under_test, "_rerun_failed_tests", side_effect=rerun),
                    patch.object(under_test, "_copy_bins_to_upload"),
                ):
                    under_test.main()

                # Evergreen recursively finds these extensions outside the Bazel output tree.
                dumps = list(Path(".").rglob("*.mdmp"))
                cores = list(Path(".").rglob("*.core.gz"))
                expected_dumps = [b"original dump 1", b"original dump 2"]
                expected_cores = [b"original core 1", b"original core 2"]
                if rerun_crashes:
                    expected_dumps.append(b"rerun dump")
                    expected_cores.append(b"rerun core")
                self.assertCountEqual([dump.read_bytes() for dump in dumps], expected_dumps)
                self.assertCountEqual(
                    [gzip.decompress(core.read_bytes()) for core in cores], expected_cores
                )
                self.assertCountEqual(
                    [core.read_bytes() for core in Path(".").rglob("*.core")],
                    [b"original plain core 1", b"original plain core 2"],
                )
                self.assertEqual(len({dump.name for dump in dumps}), len(dumps))
                self.assertEqual(len({core.name for core in cores}), len(cores))
                # Prefixing dump names must retain the PID in the position the analyzer reads.
                for core in cores:
                    self.assertEqual(core.name.removesuffix(".gz").split(".")[-2], "123")
                self.assertEqual(len(list(Path(".").rglob("other-output.txt"))), 2)
                self.assertEqual(len(list(Path(".").rglob("boring_core_dumps.txt"))), 2)


if __name__ == "__main__":
    unittest.main()
