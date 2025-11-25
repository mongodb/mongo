"""Unit tests for core dump filtering functionality."""

import os
import tempfile
import unittest
from unittest.mock import Mock, patch

from buildscripts.resmokelib.hang_analyzer.dumper import GDBDumper, filter_core_dumps


class TestCoreDumpFiltering(unittest.TestCase):
    """Unit tests for core dump filtering with boring PIDs."""

    def setUp(self):
        """Set up test fixtures."""
        self.logger = Mock()
        self.dumper = GDBDumper(self.logger, "stdout")

    def test_filter_boring_core_dumps(self):
        """Test that boring core dumps are filtered out correctly."""
        # Create a temporary directory with mock core dump files
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create mock core dump files
            interesting_cores = [
                "dump_mongod.12345.core",
                "dump_mongos.67890.core",
            ]
            boring_cores = [
                "dump_mongod.11111.core",
                "dump_mongos.22222.core",
            ]

            for core in interesting_cores + boring_cores:
                open(os.path.join(tmpdir, core), "a").close()

            # Mock the find_files function to return our test files
            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in interesting_cores + boring_cores]
                mock_find.return_value = all_cores

                # Mock analyze_core to avoid actual analysis
                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    boring_pids = {"11111", "22222"}
                    install_dir = "/mock/install"
                    analysis_dir = tmpdir
                    multiversion_dir = "/mock/multiversion"

                    report = self.dumper.analyze_cores(
                        tmpdir,
                        install_dir,
                        analysis_dir,
                        multiversion_dir,
                        "on",
                        boring_pids,
                    )

                    # Should only analyze 2 interesting cores
                    self.assertEqual(mock_analyze.call_count, 2)
                    self.assertEqual(len(report["results"]), 2)

                    # Verify that only interesting cores were analyzed
                    analyzed_files = [
                        os.path.basename(call.kwargs["core_file_path"])
                        for call in mock_analyze.call_args_list
                    ]
                    self.assertIn("dump_mongod.12345.core", analyzed_files)
                    self.assertIn("dump_mongos.67890.core", analyzed_files)
                    self.assertNotIn("dump_mongod.11111.core", analyzed_files)
                    self.assertNotIn("dump_mongos.22222.core", analyzed_files)

    def test_filter_with_empty_boring_pids(self):
        """Test that all cores are analyzed when no boring PIDs are provided."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = ["dump_mongod.12345.core", "dump_mongos.67890.core"]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    # Pass empty set of boring PIDs
                    report = self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", "on", set()
                    )

                    # Should analyze all cores
                    self.assertEqual(mock_analyze.call_count, 2)
                    self.assertEqual(len(report["results"]), 2)

    def test_filter_with_none_boring_pids(self):
        """Test that all cores are analyzed when boring PIDs is None."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = ["dump_mongod.12345.core", "dump_mongos.67890.core"]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    # Pass None for boring PIDs
                    report = self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", "on", None
                    )

                    # Should analyze all cores
                    self.assertEqual(mock_analyze.call_count, 2)
                    self.assertEqual(len(report["results"]), 2)

    def test_max_core_dumps_cap(self):
        """Test that core dump processing respects the maximum limit."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create 60 mock core dumps
            cores = [f"dump_mongod.{10000 + i}.core" for i in range(60)]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    # Should cap at 50 by default
                    report = self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", "on", None
                    )

                    # Should only analyze 50 cores (default max)
                    self.assertEqual(mock_analyze.call_count, 50)
                    self.assertEqual(len(report["results"]), 50)

    def test_max_core_dumps_custom_limit(self):
        """Test that custom max_core_dumps limit is respected."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create 30 mock core dumps
            cores = [f"dump_mongod.{10000 + i}.core" for i in range(30)]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    # Set custom max to 10
                    report = self.dumper.analyze_cores(
                        tmpdir,
                        "/mock/install",
                        tmpdir,
                        "/mock/multiversion",
                        "on",
                        None,
                        max_core_dumps=10,
                    )

                    # Should only analyze 10 cores
                    self.assertEqual(mock_analyze.call_count, 10)
                    self.assertEqual(len(report["results"]), 10)

    def test_filter_and_cap_combined(self):
        """Test that filtering and capping work together correctly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create 60 cores: 30 interesting, 30 boring
            interesting_cores = [f"dump_mongod.{10000 + i}.core" for i in range(30)]
            boring_cores = [f"dump_mongod.{20000 + i}.core" for i in range(30)]
            all_test_cores = interesting_cores + boring_cores

            for core in all_test_cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in all_test_cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    # Mark half as boring
                    boring_pids = {str(20000 + i) for i in range(30)}

                    # Set max to 20
                    report = self.dumper.analyze_cores(
                        tmpdir,
                        "/mock/install",
                        tmpdir,
                        "/mock/multiversion",
                        "on",
                        boring_pids,
                        max_core_dumps=20,
                    )

                    # Should filter out 30 boring, leaving 30 interesting
                    # Then cap at 20
                    self.assertEqual(mock_analyze.call_count, 20)
                    self.assertEqual(len(report["results"]), 20)

    def test_unparseable_core_dump_name_treated_as_interesting(self):
        """Test that core dumps with unparseable names are treated as interesting."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = [
                "dump_mongod.12345.core",  # Normal format
                "weird_name.core",  # Unparseable
                "another.strange.file.core",  # Unparseable
            ]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    boring_pids = {"12345"}

                    self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", "on", boring_pids
                    )

                    # Should analyze 2 cores (the unparseable ones are treated as interesting)
                    self.assertEqual(mock_analyze.call_count, 2)

                    # Verify that unparseable cores were analyzed
                    analyzed_files = [
                        os.path.basename(call.kwargs["core_file_path"])
                        for call in mock_analyze.call_args_list
                    ]
                    self.assertIn("weird_name.core", analyzed_files)
                    self.assertIn("another.strange.file.core", analyzed_files)
                    self.assertNotIn("dump_mongod.12345.core", analyzed_files)

    def test_multiversion_core_dump_format(self):
        """Test filtering works with multiversion core dump format."""
        with tempfile.TemporaryDirectory() as tmpdir:
            cores = [
                "dump_mongod-8.0.12345.core",  # Multiversion format with boring PID
                "dump_mongod-7.0.67890.core",  # Multiversion format with interesting PID
                "dump_mongod.11111.core",  # Normal format with interesting PID
            ]

            for core in cores:
                open(os.path.join(tmpdir, core), "a").close()

            with patch("buildscripts.resmokelib.hang_analyzer.dumper.find_files") as mock_find:
                all_cores = [os.path.join(tmpdir, c) for c in cores]
                mock_find.return_value = all_cores

                with patch.object(self.dumper, "analyze_core") as mock_analyze:
                    mock_analyze.return_value = (0, "pass")

                    boring_pids = {"12345"}

                    self.dumper.analyze_cores(
                        tmpdir, "/mock/install", tmpdir, "/mock/multiversion", "on", boring_pids
                    )

                    # Should analyze 2 interesting cores
                    self.assertEqual(mock_analyze.call_count, 2)

                    analyzed_files = [
                        os.path.basename(call.kwargs["core_file_path"])
                        for call in mock_analyze.call_args_list
                    ]
                    self.assertIn("dump_mongod-7.0.67890.core", analyzed_files)
                    self.assertIn("dump_mongod.11111.core", analyzed_files)
                    self.assertNotIn("dump_mongod-8.0.12345.core", analyzed_files)


class TestFilterCoreDumpsHelper(unittest.TestCase):
    """Unit tests for the filter_core_dumps helper function."""

    def test_no_filtering_when_no_boring_pids(self):
        """Test that all cores are returned when no boring PIDs provided."""
        core_files = ["/tmp/dump_mongod.123.core", "/tmp/dump_mongos.456.core"]
        logger = Mock()

        result = filter_core_dumps(core_files, None, 50, logger)

        self.assertEqual(result, core_files)
        self.assertEqual(len(result), 2)

    def test_filters_out_boring_pids(self):
        """Test that cores with boring PIDs are filtered out."""
        core_files = [
            "/tmp/dump_mongod.123.core",  # boring
            "/tmp/dump_mongos.456.core",  # interesting
            "/tmp/dump_mongod.789.core",  # boring
        ]
        boring_pids = {"123", "789"}
        logger = Mock()

        result = filter_core_dumps(core_files, boring_pids, 50, logger)

        self.assertEqual(len(result), 1)
        self.assertIn("/tmp/dump_mongos.456.core", result)

    def test_applies_cap(self):
        """Test that maximum cap is applied."""
        core_files = [f"/tmp/dump_mongod.{i}.core" for i in range(100)]
        logger = Mock()

        result = filter_core_dumps(core_files, None, 20, logger)

        self.assertEqual(len(result), 20)

    def test_filter_then_cap(self):
        """Test that filtering happens before capping."""
        # 10 interesting + 10 boring = 20 total
        interesting = [f"/tmp/dump_mongod.{i}.core" for i in range(10)]
        boring = [f"/tmp/dump_mongos.{i + 100}.core" for i in range(10)]
        core_files = interesting + boring
        boring_pids = {str(i + 100) for i in range(10)}
        logger = Mock()

        result = filter_core_dumps(core_files, boring_pids, 5, logger)

        # Should filter to 10 interesting, then cap at 5
        self.assertEqual(len(result), 5)
        # All results should be from interesting set
        for core in result:
            self.assertIn(core, interesting)

    def test_unparseable_filenames_treated_as_interesting(self):
        """Test that cores with unparseable names are kept."""
        core_files = [
            "/tmp/dump_mongod.123.core",  # boring, parseable
            "/tmp/weird_name.core",  # unparseable -> interesting
            "/tmp/another.file.core",  # unparseable -> interesting
        ]
        boring_pids = {"123"}
        logger = Mock()

        result = filter_core_dumps(core_files, boring_pids, 50, logger)

        self.assertEqual(len(result), 2)
        self.assertIn("/tmp/weird_name.core", result)
        self.assertIn("/tmp/another.file.core", result)
        logger.warning.assert_called()  # Should warn about unparseable names

    def test_empty_result_after_filtering(self):
        """Test handling when all cores are filtered out."""
        core_files = [
            "/tmp/dump_mongod.123.core",
            "/tmp/dump_mongos.456.core",
        ]
        boring_pids = {"123", "456"}
        logger = Mock()

        result = filter_core_dumps(core_files, boring_pids, 50, logger)

        self.assertEqual(len(result), 0)


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
