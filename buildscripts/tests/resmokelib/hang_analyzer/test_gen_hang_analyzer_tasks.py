"""Unit tests for the buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks module."""

import sys
import unittest
from unittest.mock import MagicMock, patch

from buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks import (
    GENERATED_TASK_PREFIX,
    BazelCoreAnalysisTaskGenerator,
    CoreInfo,
    ResmokeCoreAnalysisTaskGenerator,
    _get_core_analyzer_commands,
    get_core_pid,
)


class TestCorePidExtraction(unittest.TestCase):
    """Unit tests for get_core_pid function."""

    def test_standard_core_dump_format(self):
        core_file = "dump_mongod.429814.core"
        pid = get_core_pid(core_file)
        self.assertEqual(pid, "429814")

    def test_multiversion_core_dump_format(self):
        core_file = "dump_mongod-8.2.429814.core"
        pid = get_core_pid(core_file)
        self.assertEqual(pid, "429814")

    def test_with_path(self):
        core_file = "/path/to/dump_mongod.789012.core"
        pid = get_core_pid(core_file)
        self.assertEqual(pid, "789012")

    def test_invalid_format_non_digit_pid(self):
        """Test that non-digit PID raises an assertion."""
        with self.assertRaises(AssertionError):
            get_core_pid("dump_mongod.notanumber.core")


@unittest.skipIf(
    not sys.platform.startswith("linux"),
    reason="Core analysis is only support on linux",
)
class TestGetCoreAnalyzerCommands(unittest.TestCase):
    """Unit tests for get_core_analyzer_commands function."""

    def test_returns_list_of_function_calls(self):
        """Test that function returns a list."""
        commands = _get_core_analyzer_commands("task123", "0", "s3://results", "on", True, set())
        self.assertIsInstance(commands, list)
        self.assertGreater(len(commands), 0)

    def test_includes_task_id_in_subprocess_command(self):
        """Test that task ID is included in subprocess command."""
        task_id = "task_abc_123"
        commands = _get_core_analyzer_commands(task_id, "0", "s3://results", "on", True, set())

        # Find the subprocess.exec command
        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        self.assertIsNotNone(subprocess_cmd)
        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]
        self.assertIn(f"--task-id={task_id}", args)

    def test_includes_execution_in_subprocess_command(self):
        """Test that execution is included in subprocess command."""
        execution = "3"
        commands = _get_core_analyzer_commands(
            "task123", execution, "s3://results", "on", True, set()
        )

        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]
        self.assertIn(f"--execution={execution}", args)

    def test_includes_gdb_index_cache_setting(self):
        """Test that gdb index cache setting is included."""
        for cache_setting in ["on", "off"]:
            commands = _get_core_analyzer_commands(
                "task123", "0", "s3://results", cache_setting, True, set()
            )

            subprocess_cmd = None
            for cmd in commands:
                if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                    subprocess_cmd = cmd
                    break

            cmd_dict = subprocess_cmd.as_dict()
            args = cmd_dict["params"]["args"]
            self.assertIn(f"--gdb-index-cache={cache_setting}", args)

    def test_includes_boring_core_dump_pids(self):
        """Test that boring core dump PIDs are included."""
        boring_pids = {"12345", "67890", "11111"}
        commands = _get_core_analyzer_commands(
            "task123", "0", "s3://results", "on", True, boring_pids
        )

        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]

        # Find the boring PIDs argument
        boring_arg = None
        for arg in args:
            if arg and arg.startswith("--boring-core-dump-pids="):
                boring_arg = arg
                break

        self.assertIsNotNone(boring_arg)
        # Check that all PIDs are in the argument
        for pid in boring_pids:
            self.assertIn(pid, boring_arg)

    def test_empty_boring_pids(self):
        """Test handling of empty boring PIDs set."""
        commands = _get_core_analyzer_commands("task123", "0", "s3://results", "on", True, set())

        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]

        boring_arg = None
        for arg in args:
            if arg and arg.startswith("--boring-core-dump-pids="):
                boring_arg = arg
                break

        self.assertIsNotNone(boring_arg)
        self.assertEqual(boring_arg, "--boring-core-dump-pids=")

    def test_bazel_task_flag(self):
        """Test that is_bazel_task flag is passed correctly."""
        commands = _get_core_analyzer_commands(
            "task123", "0", "s3://results", "on", True, set(), is_bazel_task=True
        )

        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]
        self.assertIn("--is-bazel-task", args)

    def test_non_bazel_task_no_flag(self):
        """Test that non-bazel tasks don't include the bazel flag."""
        commands = _get_core_analyzer_commands(
            "task123", "0", "s3://results", "on", True, set(), is_bazel_task=False
        )

        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]
        # Filter out None values
        args = [arg for arg in args if arg is not None]
        self.assertNotIn("--is-bazel-task", args)

    def test_includes_s3_put_with_results_url(self):
        """Test that S3 put command includes correct results URL."""
        results_url = "s3://bucket/path/to/results.tgz"
        commands = _get_core_analyzer_commands("task123", "0", results_url, "on", True, set())

        s3_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "s3.put" in str(cmd.as_dict()):
                s3_cmd = cmd
                break

        self.assertIsNotNone(s3_cmd)
        cmd_dict = s3_cmd.as_dict()
        self.assertEqual(cmd_dict["params"]["remote_file"], results_url)

    def test_includes_otel_extra_data(self):
        """Test that OTEL extra data includes has_interesting_core_dumps flag."""
        for has_interesting in [True, False]:
            commands = _get_core_analyzer_commands(
                "task123", "0", "s3://results", "on", has_interesting, set()
            )

            subprocess_cmd = None
            for cmd in commands:
                if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                    subprocess_cmd = cmd
                    break

            cmd_dict = subprocess_cmd.as_dict()
            args = cmd_dict["params"]["args"]

            expected_str = (
                f"--otel-extra-data=has_interesting_core_dumps={str(has_interesting).lower()}"
            )
            self.assertIn(expected_str, args)


@unittest.skipIf(
    not sys.platform.startswith("linux"),
    reason="Core analysis is only support on linux",
)
class TestCoreAnalysisTaskGenerator(unittest.TestCase):
    """Unit tests for CoreAnalysisTaskGenerator base class."""

    def setUp(self):
        """Set up test fixtures."""
        self.expansions_file = "test_expansions.yml"
        self.mock_expansions = {
            "task_name": "resmoke_test",
            "task_id": "test_task_123",
            "execution": "0",
            "build_variant": "ubuntu2204",
            "distro_id": "ubuntu2204-large",
            "core_analyzer_results_url": "s3://bucket/results.tgz",
            "compile_variant": "ubuntu2204-compile",
            "workdir": "/data/mci",
        }

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    def test_generate_creates_task_config_with_interesting_cores(self, mock_read_config):
        """Test that generate creates proper task config when interesting cores are found."""
        mock_read_config.return_value = self.mock_expansions

        mock_cores = [
            CoreInfo(
                path="/tmp/dump_mongod.123.core",
                binary_name="mongod",
                pid="123",
                marked_boring=False,
            ),
            CoreInfo(
                path="/tmp/dump_mongos.456.core",
                binary_name="mongos",
                pid="456",
                marked_boring=False,
            ),
        ]

        with patch.object(ResmokeCoreAnalysisTaskGenerator, "find_cores", return_value=mock_cores):
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
            result = generator.generate()

            self.assertIsNotNone(result)
            self.assertIn("buildvariants", result)
            self.assertEqual(len(result["buildvariants"]), 1)

            variant = result["buildvariants"][0]
            self.assertEqual(variant["name"], "ubuntu2204")
            self.assertEqual(len(variant["tasks"]), 1)

            task = variant["tasks"][0]
            self.assertTrue(task["activate"])
            self.assertTrue(task["name"].startswith(GENERATED_TASK_PREFIX))

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    def test_generate_does_not_activate_with_only_boring_cores(self, mock_read_config):
        """Test that task is not activated when only boring cores are found."""
        mock_read_config.return_value = self.mock_expansions

        mock_cores = [
            CoreInfo(
                path="/tmp/dump_mongod.123.core",
                binary_name="mongod",
                pid="123",
                marked_boring=True,
            ),
            CoreInfo(
                path="/tmp/dump_mongos.456.core",
                binary_name="mongos",
                pid="456",
                marked_boring=True,
            ),
        ]

        with patch.object(ResmokeCoreAnalysisTaskGenerator, "find_cores", return_value=mock_cores):
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
            result = generator.generate()

            self.assertIsNotNone(result)
            variant = result["buildvariants"][0]
            task = variant["tasks"][0]
            self.assertFalse(task["activate"])

    def test_should_skip_task_for_hardcoded_task_names(self):
        """Test that hardcoded task names are skipped."""
        with patch(
            "buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file"
        ) as mock_read:
            mock_read.return_value = self.mock_expansions
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)

            # Test skipped task names
            for task_name in ["no_passthrough_disagg_override", "disagg_repl_jscore_passthrough"]:
                mock_task = MagicMock()
                mock_task.display_name = task_name
                mock_task.parent_task_id = None
                mock_task.build_variant = "ubuntu2204"

                self.assertTrue(generator._should_skip_task(mock_task))

    def test_should_not_skip_normal_task(self):
        """Test that normal tasks are not skipped."""
        with patch(
            "buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file"
        ) as mock_read:
            mock_read.return_value = self.mock_expansions
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)

            mock_task = MagicMock()
            mock_task.display_name = "normal_task"
            mock_task.parent_task_id = None
            mock_task.build_variant = "ubuntu2204"

            self.assertFalse(generator._should_skip_task(mock_task))


@unittest.skipIf(
    not sys.platform.startswith("linux"),
    reason="Core analysis is only support on linux",
)
class TestResmokeCoreAnalysisTaskGenerator(unittest.TestCase):
    """Unit tests for ResmokeCoreAnalysisTaskGenerator."""

    def setUp(self):
        """Set up test fixtures."""
        self.expansions_file = "test_expansions.yml"
        self.mock_expansions = {
            "task_name": "resmoke_test",
            "task_id": "test_task_123",
            "execution": "0",
            "build_variant": "ubuntu2204",
            "distro_id": "ubuntu2204-large",
            "core_analyzer_results_url": "s3://bucket/results.tgz",
            "compile_variant": "ubuntu2204-compile",
            "workdir": "/data/mci",
        }

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.path.exists")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.listdir")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.dumper.get_dumpers")
    def test_find_cores_discovers_cores_from_artifacts(
        self, mock_get_dumpers, mock_listdir, mock_exists, mock_read_config
    ):
        """Test that find_cores discovers cores from task artifacts."""
        mock_read_config.return_value = self.mock_expansions

        # Mock binary directory exists
        def exists_side_effect(path):
            if "dist-test/bin" in path or "boring_core_dumps.txt" in path:
                return True
            return False

        mock_exists.side_effect = exists_side_effect
        mock_listdir.return_value = ["mongod", "mongos"]

        # Mock task artifacts
        mock_artifact1 = MagicMock()
        mock_artifact1.name = "Core Dump 1 (dump_mongod.12345.core.gz)"
        mock_artifact2 = MagicMock()
        mock_artifact2.name = "Core Dump 2 (dump_mongos.67890.core.gz)"

        mock_task = MagicMock()
        mock_task.artifacts = [mock_artifact1, mock_artifact2]

        # Mock dumper
        mock_dbg = MagicMock()
        mock_dbg.get_binary_from_core_dump.side_effect = [
            ("mongod", None),
            ("mongos", None),
        ]
        mock_dumpers = MagicMock()
        mock_dumpers.dbg = mock_dbg
        mock_get_dumpers.return_value = mock_dumpers

        with patch("builtins.open", unittest.mock.mock_open(read_data="")):
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
            generator.evg_api.task_by_id.return_value = mock_task

            cores = generator.find_cores()

            self.assertEqual(len(cores), 2)
            self.assertEqual(cores[0].path, "dump_mongod.12345.core")
            self.assertEqual(cores[0].binary_name, "mongod")
            self.assertEqual(cores[0].pid, "12345")
            self.assertEqual(cores[1].path, "dump_mongos.67890.core")
            self.assertEqual(cores[1].binary_name, "mongos")
            self.assertEqual(cores[1].pid, "67890")

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.path.exists")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.listdir")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.dumper.get_dumpers")
    def test_find_cores_marks_boring_cores(
        self, mock_get_dumpers, mock_listdir, mock_exists, mock_read_config
    ):
        """Test that find_cores correctly marks boring cores."""
        mock_read_config.return_value = self.mock_expansions
        mock_exists.return_value = True
        mock_listdir.return_value = ["mongod"]

        # Mock artifact with boring core
        mock_artifact = MagicMock()
        mock_artifact.name = "Core Dump 1 (dump_mongod.12345.core.gz)"

        mock_task = MagicMock()
        mock_task.artifacts = [mock_artifact]

        # Mock dumper
        mock_dbg = MagicMock()
        mock_dbg.get_binary_from_core_dump.return_value = ("mongod", None)
        mock_dumpers = MagicMock()
        mock_dumpers.dbg = mock_dbg
        mock_get_dumpers.return_value = mock_dumpers

        # Mock boring PIDs file with PID 12345
        with patch("builtins.open", unittest.mock.mock_open(read_data="12345\n67890\n")):
            generator = ResmokeCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
            generator.evg_api.task_by_id.return_value = mock_task

            cores = generator.find_cores()

            self.assertEqual(len(cores), 1)
            self.assertTrue(cores[0].marked_boring)


@unittest.skipIf(
    not sys.platform.startswith("linux"),
    reason="Core analysis is only support on linux",
)
class TestBazelCoreAnalysisTaskGenerator(unittest.TestCase):
    """Unit tests for BazelCoreAnalysisTaskGenerator."""

    def setUp(self):
        """Set up test fixtures."""
        self.expansions_file = "test_expansions.yml"
        self.mock_expansions = {
            "task_name": "bazel_test",
            "task_id": "test_task_123",
            "execution": "0",
            "build_variant": "ubuntu2204",
            "distro_id": "ubuntu2204-large",
            "core_analyzer_results_url": "s3://bucket/results.tgz",
            "compile_variant": "ubuntu2204-compile",
            "workdir": "/data/mci",
        }

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.path.exists")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.glob.glob")
    def test_find_cores_discovers_cores_in_test_outputs(
        self, mock_glob, mock_exists, mock_read_config
    ):
        """Test that find_cores discovers cores in test.outputs directories."""
        mock_read_config.return_value = self.mock_expansions

        def exists_side_effect(path):
            if "results" in path and "boring_core_dumps.txt" not in path:
                return True
            return False

        mock_exists.side_effect = exists_side_effect

        # Mock glob to return test.outputs directories
        def glob_side_effect(pattern, **kwargs):
            if ".core" in pattern:
                if "test1" in pattern:
                    return ["/data/mci/results/test1/test.outputs/dump_mongod.12345.core"]
                elif "test2" in pattern:
                    return ["/data/mci/results/test2/test.outputs/dump_mongos.67890.core"]
            elif ".mdmp" in pattern:
                return []
            elif "test.outputs" in pattern and "recursive" in kwargs:
                return [
                    "/data/mci/results/test1/test.outputs",
                    "/data/mci/results/test2/test.outputs",
                ]
            return []

        mock_glob.side_effect = glob_side_effect

        generator = BazelCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
        cores = generator.find_cores()

        self.assertEqual(len(cores), 2)
        self.assertIn("dump_mongod.12345.core", cores[0].path)
        self.assertEqual(cores[0].pid, "12345")
        self.assertIn("dump_mongos.67890.core", cores[1].path)
        self.assertEqual(cores[1].pid, "67890")

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.os.path.exists")
    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.glob.glob")
    def test_find_cores_marks_boring_cores_from_test_outputs(
        self, mock_glob, mock_exists, mock_read_config
    ):
        """Test that find_cores marks boring cores based on boring_core_dumps.txt."""
        mock_read_config.return_value = self.mock_expansions

        boring_file_path = None

        def exists_side_effect(path):
            if "results" in path and "test.outputs" not in path:
                return True
            if "boring_core_dumps.txt" in path:
                nonlocal boring_file_path
                boring_file_path = path
                return True
            return False

        mock_exists.side_effect = exists_side_effect

        def glob_side_effect(pattern, **kwargs):
            if ".core" in pattern:
                return ["/data/mci/results/test1/test.outputs/dump_mongod.12345.core"]
            elif ".mdmp" in pattern:
                return []
            elif "test.outputs" in pattern and "recursive" in kwargs:
                return ["/data/mci/results/test1/test.outputs"]
            return []

        mock_glob.side_effect = glob_side_effect

        # Mock boring PIDs file
        with patch("builtins.open", unittest.mock.mock_open(read_data="12345\n")):
            generator = BazelCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)
            cores = generator.find_cores()

            self.assertEqual(len(cores), 1)
            self.assertTrue(cores[0].marked_boring)

    @patch("buildscripts.resmokelib.hang_analyzer.gen_hang_analyzer_tasks.read_config_file")
    def test_get_core_analyzer_commands_includes_bazel_flag(self, mock_read_config):
        """Test that get_core_analyzer_commands includes bazel flag."""
        mock_read_config.return_value = self.mock_expansions

        generator = BazelCoreAnalysisTaskGenerator(self.expansions_file, use_mock_tasks=True)

        commands = generator.get_core_analyzer_commands(
            "task123", "0", "s3://results", "on", True, set()
        )

        # Find subprocess command and verify it has --is-bazel-task flag
        subprocess_cmd = None
        for cmd in commands:
            if hasattr(cmd, "as_dict") and "subprocess.exec" in str(cmd.as_dict()):
                subprocess_cmd = cmd
                break

        self.assertIsNotNone(subprocess_cmd)
        cmd_dict = subprocess_cmd.as_dict()
        args = cmd_dict["params"]["args"]
        self.assertIn("--is-bazel-task", args)


if __name__ == "__main__":
    unittest.main()
