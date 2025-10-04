"""Unit tests for buildscripts/resmokelib/testing/symbolizer_service.py."""

import os
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import MagicMock

from buildscripts.resmokelib.testing import symbolizer_service as under_test


def mock_resmoke_symbolizer_config():
    config_mock: under_test.ResmokeSymbolizerConfig = MagicMock(
        spec_set=under_test.ResmokeSymbolizerConfig
    )
    config_mock.evg_task_id = "evg_task_id"
    config_mock.client_id = "client_id"
    config_mock.client_secret = "client_secret"
    config_mock.skip_symbolization = False
    config_mock.is_windows.return_value = False
    config_mock.is_macos.return_value = False
    return config_mock


class TestResmokeSymbolizer(unittest.TestCase):
    def setUp(self) -> None:
        self.config_mock = mock_resmoke_symbolizer_config()
        self.symbolizer_service_mock: under_test.SymbolizerService = MagicMock(
            spec_set=under_test.SymbolizerService
        )
        self.file_service_mock: under_test.FileService = MagicMock(spec_set=under_test.FileService)
        self.resmoke_symbolizer = under_test.ResmokeSymbolizer(
            self.config_mock, self.symbolizer_service_mock, self.file_service_mock
        )

    def test_symbolize_test_logs_should_not_symbolize(self):
        self.config_mock.is_windows.return_value = True

        self.resmoke_symbolizer.get_unsymbolized_stacktrace(MagicMock())
        self.symbolizer_service_mock.run_symbolizer_script.assert_not_called()

    def test_symbolize_test_logs_could_not_get_dbpath(self):
        self.file_service_mock.check_path_exists.return_value = False

        self.resmoke_symbolizer.get_unsymbolized_stacktrace(MagicMock())
        self.symbolizer_service_mock.run_symbolizer_script.assert_not_called()

    def test_symbolize_test_logs_did_not_find_files(self):
        self.file_service_mock.find_all_children_recursively.return_value = []

        self.resmoke_symbolizer.get_unsymbolized_stacktrace(MagicMock())
        self.symbolizer_service_mock.run_symbolizer_script.assert_not_called()

    def test_should_not_symbolize_if_not_in_evergreen(self):
        self.config_mock.evg_task_id = None
        ret = self.resmoke_symbolizer.should_symbolize(MagicMock())
        self.assertFalse(ret)

    def test_should_not_symbolize_if_secrets_are_absent(self):
        self.config_mock.client_id = None
        self.config_mock.client_secret = None
        ret = self.resmoke_symbolizer.should_symbolize(MagicMock())
        self.assertFalse(ret)

    def test_should_not_symbolize_if_on_windows(self):
        self.config_mock.is_windows.return_value = True
        ret = self.resmoke_symbolizer.should_symbolize(MagicMock())
        self.assertFalse(ret)

    def test_should_not_symbolize_if_on_macos(self):
        self.config_mock.is_macos.return_value = True
        ret = self.resmoke_symbolizer.should_symbolize(MagicMock())
        self.assertFalse(ret)

    def test_should_symbolize_return_true(self):
        ret = self.resmoke_symbolizer.should_symbolize(MagicMock())
        self.assertTrue(ret)

    def test_get_stacktrace_dir_returns_dir(self):
        dbpath = "dbpath"
        test = MagicMock(fixture=MagicMock(get_dbpath_prefix=MagicMock(return_value=dbpath)))
        self.file_service_mock.check_path_exists.return_value = True

        ret = self.resmoke_symbolizer.get_stacktrace_dir(test)
        self.assertEqual(ret, dbpath)

    def test_get_stacktrace_dir_if_dir_does_not_exist(self):
        test = MagicMock()
        self.file_service_mock.check_path_exists.return_value = False

        ret = self.resmoke_symbolizer.get_stacktrace_dir(test)
        self.assertEqual(ret, None)

    def test_get_stacktrace_dir_if_fixture_is_not_available(self):
        test = MagicMock(fixture=None)

        ret = self.resmoke_symbolizer.get_stacktrace_dir(test)
        self.assertEqual(ret, None)


class TestFileService(unittest.TestCase):
    def setUp(self) -> None:
        self.file_service = under_test.FileService()
        self.relative_dir_paths = [
            os.path.join("dir_1"),
            os.path.join("dir_2", "dir_2_1"),
            os.path.join("dir_2", "dir_2_2"),
            os.path.join("dir_3"),
        ]
        self.relative_file_paths = [
            os.path.join("dir_1", "file_1.stacktrace"),
            os.path.join("dir_2", "file_2.stacktrace"),
            os.path.join("dir_2", "dir_2_1", "file_3.stacktrace"),
            os.path.join("dir_2", "dir_2_2", "file_4.stacktrace"),
        ]

    def test_find_all_children_recursively_returns_files(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            for file in abs_file_paths:
                Path(file).touch()

            ret = self.file_service.find_all_children_recursively(tmpdir)
            self.assertListEqual(sorted(ret), sorted(abs_file_paths))

    def test_find_all_children_recursively_no_files(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)

            ret = self.file_service.find_all_children_recursively(tmpdir)
            self.assertListEqual(ret, [])

    def test_find_all_children_recursively_no_dirs(self):
        with TemporaryDirectory() as tmpdir:
            ret = self.file_service.find_all_children_recursively(tmpdir)
            self.assertListEqual(ret, [])

    def test_filter_out_non_files_if_all_files_absent(self):
        with TemporaryDirectory() as tmpdir:
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            ret = self.file_service.filter_out_non_files(abs_file_paths)
            self.assertListEqual(ret, [])

    def test_filter_out_non_files_if_some_files_present(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            Path(abs_file_paths[1]).touch()
            Path(abs_file_paths[3]).touch()

            ret = self.file_service.filter_out_non_files(abs_file_paths)
            self.assertListEqual(ret, [abs_file_paths[1], abs_file_paths[3]])

    def test_filter_out_non_files_if_all_files_present(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            for file in abs_file_paths:
                Path(file).touch()

            ret = self.file_service.filter_out_non_files(abs_file_paths)
            self.assertListEqual(ret, abs_file_paths)

    def test_remove_empty_files_if_no_empty(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            for file in abs_file_paths:
                with open(file, "w") as fstream:
                    fstream.write("stacktrace")

            self.assertEqual(
                set(self.file_service.filter_out_empty_files(abs_file_paths)), set(abs_file_paths)
            )

    def test_do_not_panic_when_file_does_not_exist(self):
        non_existing_files = ["this-does-not-exist.file", "my.cat"]
        # non-existing files should be filtered out, instead of causing errors
        self.assertListEqual(self.file_service.filter_out_empty_files(non_existing_files), [])

    def test_filter_out_empty_files_if_partly_empty(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            with open(abs_file_paths[0], "w") as fstream:
                fstream.write("stacktrace")
            Path(abs_file_paths[1]).touch()
            with open(abs_file_paths[2], "w") as fstream:
                fstream.write("stacktrace")
            Path(abs_file_paths[3]).touch()

            filtered = self.file_service.filter_out_empty_files(abs_file_paths)

            self.assertTrue(abs_file_paths[0] in filtered)
            self.assertFalse(abs_file_paths[1] in filtered)
            self.assertTrue(abs_file_paths[2] in filtered)
            self.assertFalse(abs_file_paths[3] in filtered)

    def test_filter_out_empty_files_if_all_empty(self):
        with TemporaryDirectory() as tmpdir:
            abs_dir_paths = [os.path.join(tmpdir, d) for d in self.relative_dir_paths]
            abs_file_paths = [os.path.join(tmpdir, f) for f in self.relative_file_paths]
            for dir_ in abs_dir_paths:
                Path(dir_).mkdir(parents=True)
            for file in abs_file_paths:
                Path(file).touch()

            filtered = self.file_service.filter_out_empty_files(abs_file_paths)
            for file in abs_file_paths:
                self.assertFalse(file in filtered)

    def test_filter_out_already_processed_files(self):
        processed_files = ["processed-file.stacktrace"]
        files = [
            "file.stacktrace",
            "other-file.stacktrace",
            "another-file.stacktrace",
            *processed_files,
        ]
        self.file_service.add_to_processed_files(processed_files)
        filtered = self.file_service.filter_out_already_processed_files(files)
        self.assertTrue(all(file not in processed_files for file in filtered))
