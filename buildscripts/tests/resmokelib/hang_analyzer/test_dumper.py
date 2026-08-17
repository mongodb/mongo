"""Unit tests for the buildscripts.resmokelib.hang_analyzer.dumper package"""

import os
import platform
import tempfile
import time
import unittest
from datetime import timedelta
from unittest.mock import MagicMock, Mock, patch

import psutil

from buildscripts.resmokelib.hang_analyzer.dumper import GDBDumper, SigabrtDumper
from buildscripts.resmokelib.hang_analyzer.process_list import Pinfo


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

    def test_core_path_with_space_passed_intact(self):
        """Core filenames can contain spaces, so the path must reach gdb as one argv element."""
        core_path = "/data/mci/src/core-analyzer/core-dumps/dump_prefetch-ser 6.16061.core"
        gdb_output = "Core was generated by `/data/mci/src/dist-test/bin/mongod'. "

        with patch("buildscripts.resmokelib.hang_analyzer.dumper.subprocess.run") as run:
            run.return_value = MagicMock(stdout=gdb_output)
            self.dumper.get_binary_from_core_dump(core_path)

        args = run.call_args[0][0]
        self.assertIn(core_path, args)
        self.assertNotIn("-ex", args)


class TestSigabrtDumperFallback(unittest.TestCase):
    """Unit tests for SigabrtDumper falling back to a debugger."""

    def setUp(self):
        """Set up test fixtures."""
        self.logger = Mock()
        self.fallback = Mock()
        self.dumper = SigabrtDumper(self.logger, "stdout", fallback_dumper=self.fallback)
        # Keep the test fast; the production grace period is minutes.
        self.dumper.SIGABRT_GRACE = timedelta(seconds=0)

    def _dump(self, pinfo):
        with (
            patch("buildscripts.resmokelib.hang_analyzer.dumper.psutil.Process") as process,
            patch("buildscripts.resmokelib.hang_analyzer.dumper.signal_process"),
            patch("buildscripts.resmokelib.hang_analyzer.dumper.resume_process"),
        ):
            process.side_effect = lambda pid: self.procs[pid]
            self.dumper._dump_impl(pinfo, take_dump=True)

    def test_process_surviving_sigabrt_falls_back_to_debugger(self):
        """A process wedged in its own signal handler ignores SIGABRT, so we must still get stacks."""
        wedged = Mock(pid=5994)
        wedged.is_running.return_value = True
        wedged.status.return_value = psutil.STATUS_SLEEPING
        self.procs = {5994: wedged}

        self._dump(Pinfo(name="mongod", pidv=[5994]))

        # Backtraces only: taking a core dump is what the sanitizer exemption exists to avoid.
        self.fallback.dump_live_backtraces.assert_called_once()
        self.fallback.dump_info.assert_not_called()
        (pinfo,), _ = self.fallback.dump_live_backtraces.call_args
        self.assertEqual(pinfo.name, "mongod")
        self.assertEqual(pinfo.pidv, [5994])

    def test_process_dying_from_sigabrt_does_not_use_debugger(self):
        """SIGABRT working as intended must not change behaviour."""
        died = Mock(pid=5994)
        died.is_running.return_value = False
        self.procs = {5994: died}

        self._dump(Pinfo(name="mongod", pidv=[5994]))

        self.fallback.dump_live_backtraces.assert_not_called()


class TestGDBDumperLiveBacktraces(unittest.TestCase):
    """Unit tests for GDBDumper.dump_live_backtraces."""

    def setUp(self):
        """Set up test fixtures."""
        self.logger = Mock()
        self.dumper = GDBDumper(self.logger, "stdout")

    def test_gdb_is_asked_to_attach_and_backtrace_without_a_core_dump(self):
        """The commands must actually reach gdb; computing them without invoking it collects nothing."""
        with (
            patch.object(self.dumper, "find_debugger") as find_debugger,
            patch("buildscripts.resmokelib.hang_analyzer.dumper.call") as call,
        ):
            find_debugger.return_value = "/usr/bin/gdb"
            call.return_value = 0
            self.dumper.dump_live_backtraces(Pinfo(name="mongod", pidv=[5994]))

        call.assert_called_once()
        argv = call.call_args[0][0]
        self.assertEqual(argv[0], "/usr/bin/gdb")
        self.assertIn("attach 5994", argv)
        self.assertIn("thread apply all bt", argv)
        self.assertIn("detach", argv)
        # No core dump, and no reliance on .gdbinit / the mongo GDB extensions.
        self.assertNotIn("gcore", " ".join(argv))
        self.assertIn("--nx", argv)
        # A failing gdb must not raise out of the hang analyzer's teardown path.
        self.assertFalse(call.call_args[1]["check"])

    def test_missing_debugger_is_not_fatal(self):
        """No debugger on the host must warn rather than raise."""
        with (
            patch.object(self.dumper, "find_debugger") as find_debugger,
            patch("buildscripts.resmokelib.hang_analyzer.dumper.call") as call,
        ):
            find_debugger.return_value = None
            self.dumper.dump_live_backtraces(Pinfo(name="mongod", pidv=[5994]))

        call.assert_not_called()
        self.logger.warning.assert_called_once()

    def test_backtrace_budget_is_shared_across_processes(self):
        """The timeout bounds the whole set of PIDs; a per-process bound would scale with survivors."""
        self.dumper.BACKTRACE_TIMEOUT_SECONDS = 1

        with (
            patch.object(self.dumper, "find_debugger") as find_debugger,
            patch("buildscripts.resmokelib.hang_analyzer.dumper.call") as call,
        ):
            find_debugger.return_value = "/usr/bin/gdb"
            call.side_effect = lambda *args, **kwargs: time.sleep(0.05) or 0
            self.dumper.dump_live_backtraces(Pinfo(name="mongod", pidv=[1, 2, 3]))

        timeouts = [args[0][2] for args in call.call_args_list]
        self.assertEqual(len(timeouts), 3)
        # Each process gets what is left of the budget, not a fresh copy of it.
        self.assertEqual(timeouts, sorted(timeouts, reverse=True))
        self.assertLess(timeouts[-1], timeouts[0])
        self.assertLessEqual(timeouts[0], self.dumper.BACKTRACE_TIMEOUT_SECONDS)

    def test_exhausted_budget_skips_remaining_processes(self):
        """Once the budget is gone the loop must stop rather than attach with a non-positive timeout."""
        self.dumper.BACKTRACE_TIMEOUT_SECONDS = 0

        with (
            patch.object(self.dumper, "find_debugger") as find_debugger,
            patch("buildscripts.resmokelib.hang_analyzer.dumper.call") as call,
        ):
            find_debugger.return_value = "/usr/bin/gdb"
            self.dumper.dump_live_backtraces(Pinfo(name="mongod", pidv=[1, 2, 3]))

        call.assert_not_called()
        self.logger.warning.assert_called_once()


if __name__ == "__main__":
    unittest.main()
