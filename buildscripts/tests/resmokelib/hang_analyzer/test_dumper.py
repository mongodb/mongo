"""Unit tests for the buildscripts.resmokelib.hang_analyzer.dumper package"""

import os
import platform
import tempfile
import unittest
from unittest.mock import MagicMock, Mock, patch

from buildscripts.resmokelib.hang_analyzer.dumper import GDBDumper


class TestGDBDumperAnalyzeCores(unittest.TestCase):
    """Unit tests for GDBDumper.analyze_cores."""

    def setUp(self):
        """Set up test fixtures."""
        self.logger = Mock()
        self.dumper = GDBDumper(self.logger, "stdout")

    def test_unparseable_core_dump_name_analyzed(self):
        """Test that core dumps with unparseable names are still analyzed."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = [
                "dump_mongod.12345.core",  # Normal format
                "weird_name.core",  # Unparseable
                "another.strange.file.core",  # Unparseable
            ]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                mock_find.return_value = [os.path.join(tmpdir, c) for c in cores]

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    report = self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", None, "on"
                    )

                    self.assertEqual(len(report["results"]), 3)
                    self.assertEqual(mock_analyze.call_count, 3)
                    analyzed_files = [
                        os.path.basename(call.kwargs["core_file_path"])
                        for call in mock_analyze.call_args_list
                    ]
                    self.assertIn("dump_mongod.12345.core", analyzed_files)
                    self.assertIn("weird_name.core", analyzed_files)
                    self.assertIn("another.strange.file.core", analyzed_files)

    def test_multiversion_core_dump_format(self):
        """Test that multiversion-format core dump filenames are analyzed correctly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = [
                "dump_mongod-8.0.12345.core",  # Multiversion format
                "dump_mongod-7.0.67890.core",  # Multiversion format
                "dump_mongod.11111.core",  # Normal format
            ]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                mock_find.return_value = [os.path.join(tmpdir, c) for c in cores]

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    report = self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", None, "on"
                    )

                    self.assertEqual(len(report["results"]), 3)
                    self.assertEqual(mock_analyze.call_count, 3)
                    analyzed_files = [
                        os.path.basename(call.kwargs["core_file_path"])
                        for call in mock_analyze.call_args_list
                    ]
                    self.assertIn("dump_mongod-8.0.12345.core", analyzed_files)
                    self.assertIn("dump_mongod-7.0.67890.core", analyzed_files)
                    self.assertIn("dump_mongod.11111.core", analyzed_files)


class TestPIDParsing(unittest.TestCase):
    """Unit tests for PID parsing from command line arguments."""

    def test_parse_empty_string(self):
        """Test parsing empty string returns empty set."""
        boring_pids_str = ""
        if boring_pids_str:
            result = set(pid for pid in boring_pids_str.split(",") if pid)
        else:
            result = set()
        self.assertEqual(result, set())

    def test_parse_single_pid(self):
        """Test parsing single PID."""
        boring_pids_str = "12345"
        result = set(pid for pid in boring_pids_str.split(",") if pid)
        self.assertEqual(result, {"12345"})

    def test_parse_multiple_pids(self):
        """Test parsing multiple PIDs."""
        boring_pids_str = "12345,67890,11111"
        result = set(pid for pid in boring_pids_str.split(",") if pid)
        self.assertEqual(result, {"12345", "67890", "11111"})

    def test_parse_with_empty_elements(self):
        """Test parsing handles empty elements (trailing/leading commas)."""
        boring_pids_str = ",12345,67890,"
        result = set(pid for pid in boring_pids_str.split(",") if pid)
        self.assertEqual(result, {"12345", "67890"})

    def test_parse_consecutive_commas(self):
        """Test parsing handles consecutive commas."""
        boring_pids_str = "12345,,67890"
        result = set(pid for pid in boring_pids_str.split(",") if pid)
        self.assertEqual(result, {"12345", "67890"})

    def test_parse_only_commas(self):
        """Test parsing only commas returns empty set."""
        boring_pids_str = ",,,"
        result = set(pid for pid in boring_pids_str.split(",") if pid)
        self.assertEqual(result, set())


@unittest.skipUnless(platform.system() == "Linux", "GDBDumper is only for linux.")
class TestBinaryParsing(unittest.TestCase):
    def setUp(self):
        self.logger = Mock()
        self.dumper = GDBDumper(self.logger, "stdout")

    def _get_binary_from_core_dump(self, gdb_output):
        with patch("buildscripts.resmokelib.hang_analyzer.dumper.subprocess.run") as run:
            run.return_value = MagicMock(stdout=gdb_output)
            return self.dumper.get_binary_from_core_dump("core")

    def test_no_version(self):
        gdb_output = """
                     Core was generated by `/data/mci/56724897cdbfea2f5acb1cdd0b2556a6/src/dist-test/bin/mongod --someArg'. 
                     """
        name, version = self._get_binary_from_core_dump(gdb_output)
        self.assertEqual(name, "mongod")
        self.assertEqual(version, None)

    def test_binary_version(self):
        gdb_output = """
                     Core was generated by `/data/mci/56724897cdbfea2f5acb1cdd0b2556a6/src/dist-test/bin/mongod-8.0'. 
                     """
        name, version = self._get_binary_from_core_dump(gdb_output)
        self.assertEqual(name, "mongod-8.0")
        self.assertEqual(version, "8.0")

    def test_multiline(self):
        gdb_output = """
                     Core was generated by `/data/mci/56724897cdbfea2f5acb1cdd0b2556a6/src/dist-test/bin/mongo 
                     jstests/core/query/query_settings/query_settings_index_application_distinct.js'.
                     """
        name, version = self._get_binary_from_core_dump(gdb_output)
        self.assertEqual(name, "mongo")
        self.assertEqual(version, None)


if __name__ == "__main__":
    unittest.main()
