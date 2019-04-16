""" Unit tests for adb_monitor. """
# pylint: disable=protected-access,no-self-use,missing-docstring,too-many-public-methods

import os
import unittest

from unittest.mock import MagicMock, mock_open, patch

import buildscripts.mobile.adb_monitor as adb_monitor

ADB_MONITOR = "buildscripts.mobile.adb_monitor"


def ns(module):
    """Get the namespace."""
    # pylint: disable=invalid-name
    return f"{ADB_MONITOR}.{module}"


class TestParseCommandLine(unittest.TestCase):
    def test_parse_command_line(self):
        options = adb_monitor.parse_command_line().parse_args([])
        self.assertEqual(options.adb_binary, adb_monitor.DEFAULT_ADB_BINARY)
        self.assertEqual(options.python27, adb_monitor.DEFAULT_PYTHON27)
        self.assertEqual(options.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)
        self.assertEqual(options.log_level, adb_monitor.DEFAULT_LOG_LEVEL)
        self.assertEqual(options.battery_file, adb_monitor.DEFAULT_BATTERY_FILE)
        self.assertEqual(options.memory_file, adb_monitor.DEFAULT_MEMORY_FILE)
        self.assertEqual(options.cpu_file, adb_monitor.DEFAULT_CPU_FILE)
        self.assertEqual(options.num_samples, adb_monitor.DEFAULT_NUM_SAMPLES)
        self.assertEqual(options.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)
        self.assertIsNone(options.collection_time_secs)

    def test_parse_command_line_partial_args(self):
        adb_binary = "myadb"
        python27 = "mypython2"
        collection_time_secs = 35
        battery_file = "mybattery"
        arg_list = [
            "--adbBinary", adb_binary, "--python27", python27, "--collectionTime",
            str(collection_time_secs), "--batteryFile", battery_file, "--noMemory"
        ]
        options = adb_monitor.parse_command_line().parse_args(arg_list)
        self.assertEqual(options.adb_binary, adb_binary)
        self.assertEqual(options.python27, python27)
        self.assertEqual(options.collection_time_secs, collection_time_secs)
        self.assertEqual(options.battery_file, battery_file)

        self.assertEqual(options.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)
        self.assertEqual(options.log_level, adb_monitor.DEFAULT_LOG_LEVEL)
        self.assertIsNone(options.memory_file)
        self.assertEqual(options.cpu_file, adb_monitor.DEFAULT_CPU_FILE)
        self.assertEqual(options.num_samples, adb_monitor.DEFAULT_NUM_SAMPLES)
        self.assertEqual(options.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)

    def test_parse_command_line_no_files(self):
        arg_list = ["--noBattery", "--noCpu", "--noMemory"]
        options = adb_monitor.parse_command_line().parse_args(arg_list)
        self.assertIsNone(options.battery_file)
        self.assertIsNone(options.cpu_file)
        self.assertIsNone(options.memory_file)

    def test_parse_command_line_no_battery_first(self):
        battery_file = "mybattery"
        arg_list = ["--noBattery", "--batteryFile", battery_file]
        options = adb_monitor.parse_command_line().parse_args(arg_list)
        self.assertEqual(options.battery_file, battery_file)

    def test_parse_command_line_no_battery_second(self):
        battery_file = "mybattery"
        arg_list = ["--batteryFile", battery_file, "--noBattery"]
        options = adb_monitor.parse_command_line().parse_args(arg_list)
        self.assertIsNone(options.battery_file)


class TestMonitorDevice(unittest.TestCase):
    @patch(ns("fileops"), return_value=False)
    def test_monitor_device(self, mock_fileops):
        """Basic test monitor device."""
        files_mtime = {"file1": 0, "file2": 0}
        mock_fileops.getmtime.return_value = 10
        mock_adb_control = MagicMock()
        adb_monitor.monitor_device(mock_adb_control, files_mtime)
        mock_adb_control.start.assert_called_once()
        mock_adb_control.wait.assert_called_once()
        self.assertEqual(mock_fileops.getmtime.call_count, len(files_mtime))
        self.assertEqual(mock_fileops.is_empty.call_count, len(files_mtime))

    @patch(ns("fileops"), return_value=True)
    def test_monitor_device_empty_file(self, mock_fileops):
        files_mtime = {"file1": 0, "file2": 0}
        mock_fileops.getmtime.return_value = 10
        mock_adb_control = MagicMock()
        adb_monitor.monitor_device(mock_adb_control, files_mtime)
        mock_adb_control.start.assert_called_once()
        mock_adb_control.wait.assert_called_once()
        self.assertEqual(mock_fileops.getmtime.call_count, len(files_mtime))
        self.assertEqual(mock_fileops.is_empty.call_count, len(files_mtime))

    @patch(ns("fileops"), return_value=False)
    def test_monitor_device_earlier_mtime(self, mock_fileops):
        files_mtime = {"file1": 10, "file2": 10}
        mock_fileops.getmtime.return_value = 6
        mock_adb_control = MagicMock()
        adb_monitor.monitor_device(mock_adb_control, files_mtime)
        mock_adb_control.start.assert_called_once()
        mock_adb_control.wait.assert_called_once()
        self.assertEqual(mock_fileops.getmtime.call_count, len(files_mtime))
        mock_fileops.is_empty.assert_not_called()

    @patch(ns("fileops"), return_value=False)
    def test_monitor_device_no_files(self, mock_fileops):
        files_mtime = {}
        mock_fileops.getmtime.return_value = 10
        mock_adb_control = MagicMock()
        adb_monitor.monitor_device(mock_adb_control, files_mtime)
        mock_adb_control.start.assert_called_once()
        mock_adb_control.wait.assert_called_once()
        mock_fileops.getmtime.assert_not_called()
        mock_fileops.is_empty.assert_not_called()


class TestOutputFilesMtime(unittest.TestCase):
    @patch(ns("fileops.getmtime"))
    def test_output_files_mtime(self, mock_getmtime):
        mtime = 11
        mock_getmtime.return_value = mtime
        files = ["file1", "file2"]
        m_files = adb_monitor.create_files_mtime(files)
        self.assertEqual(len(m_files), len(files))
        for fn in files:
            self.assertEqual(m_files[fn], mtime)

    @patch(ns("fileops.getmtime"))
    def test_output_files_mtime_no_files(self, mock_getmtime):
        # pylint: disable=unused-argument
        m_files = adb_monitor.create_files_mtime([])
        self.assertEqual(len(m_files), 0)


class TestFindExecutable(unittest.TestCase):
    def test_find_executable(self):
        my_binary = "mybinary"
        with patch(ns("distutils.spawn.find_executable"), side_effect=lambda x: x):
            adb_binary = adb_monitor.find_executable(my_binary)
            self.assertEqual(adb_binary, my_binary)

    def test_find_executable_not_found(self):
        with patch(ns("distutils.spawn.find_executable"), side_effect=lambda x: x),\
             self.assertRaises(EnvironmentError):
            adb_monitor.find_executable(None)


class TestAdb(unittest.TestCase):
    @patch(ns("os.path.isfile"), return_value=True)
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test___init__(self, mock_find_executable, mock_isfile):
        # pylint: disable=unused-argument
        os_path = os.environ["PATH"]
        adb = adb_monitor.Adb()
        self.assertTrue(adb.systrace_script.startswith(os.path.join("systrace", "systrace.py")))
        self.assertEqual(adb.logger, adb_monitor.LOGGER)
        self.assertEqual(adb.python27, adb_monitor.DEFAULT_PYTHON27)
        self.assertEqual(adb.logger, adb_monitor.LOGGER)
        self.assertEqual(os.environ["PATH"], os_path)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test___init__adb_binary(self, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        adb_dir = os.path.join("/root", "adb_dir")
        adb_path = os.path.join(adb_dir, "adb")
        mock_os.environ = {"PATH": os.environ["PATH"]}
        mock_os.path.pathsep = os.path.pathsep
        mock_os.path.dirname = lambda x: x
        adb = adb_monitor.Adb(adb_binary=adb_path)
        self.assertTrue(adb.systrace_script.startswith(adb_dir))
        self.assertIn(os.path.pathsep + adb_dir, mock_os.environ["PATH"])

    @patch(ns("os.environ"))
    @patch(ns("os.path.isfile"), return_value=True)
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test___init__python27_binary(self, mock_find_executable, mock_isfile, mock_os_environ):
        # pylint: disable=unused-argument
        python_dir = os.path.join("/root", "python27_dir")
        python27_path = os.path.join(python_dir, "python2")
        adb = adb_monitor.Adb(python27=python27_path)
        self.assertEqual(adb.python27, python27_path)

    @patch(ns("os.environ"))
    @patch(ns("os.path.isfile"), return_value=False)
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test___init__bad_systrace(self, mock_find_executable, mock_isfile, mock_os_environ):
        # pylint: disable=unused-argument
        with self.assertRaises(EnvironmentError):
            adb_monitor.Adb()

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_adb_cmd_output(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        adb_result = adb_monitor.Adb.adb_cmd("mycmd")
        self.assertEqual(adb_result, mock_runcmd.RunCommand().execute_with_output())
        self.assertNotEqual(adb_result, mock_runcmd.RunCommand().execute_save_output())

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_adb_cmd_output_string(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        adb_result = adb_monitor.Adb.adb_cmd("mycmd", output_string=True)
        self.assertEqual(adb_result, mock_runcmd.RunCommand().execute_with_output())
        self.assertNotEqual(adb_result, mock_runcmd.RunCommand().execute_save_output())

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_adb_cmd_save_output(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        adb_result = adb_monitor.Adb.adb_cmd("mycmd", output_file="myfile")
        self.assertEqual(adb_result, mock_runcmd.RunCommand().execute_save_output())
        self.assertNotEqual(adb_result, mock_runcmd.RunCommand().execute_with_output())

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_adb_cmd_all_params(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        adb_result = adb_monitor.Adb.adb_cmd("mycmd", output_file="myfile", append_file=True,
                                             output_string=True)
        self.assertNotEqual(adb_result, mock_runcmd.RunCommand().execute_save_output())
        self.assertEqual(adb_result, mock_runcmd.RunCommand().execute_with_output())

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_shell(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        cmd_output = adb_monitor.Adb.shell("mycmd")
        self.assertEqual(cmd_output, mock_runcmd.RunCommand().execute_with_output())

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_shell_stripped(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        output = "output from shell"
        mock_runcmd.RunCommand().execute_with_output.return_value = output + "__EXIT__:0\n"
        cmd_output = adb_monitor.Adb.shell("mycmd")
        self.assertEqual(cmd_output, output)

    @patch(ns("os"))
    @patch(ns("runcommand"))
    def test_shell_error(self, mock_runcmd, mock_os):
        # pylint: disable=unused-argument
        output = "output from shell"
        mock_runcmd.RunCommand().execute_with_output.return_value = output + "__EXIT__:1\n"
        with self.assertRaises(RuntimeError):
            adb_monitor.Adb.shell("mycmd")

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_devices(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.devices()
        mock_runcmd.RunCommand.assert_called_once_with("adb devices -l", unittest.mock.ANY,
                                                       unittest.mock.ANY)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_device_available(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.device_available()
        mock_runcmd.RunCommand.assert_called_once_with("adb shell uptime", unittest.mock.ANY,
                                                       unittest.mock.ANY)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_push(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        files = "myfile"
        remote_dir = "/remotedir"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.push(files, remote_dir)
        push_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        self.assertIn(files, push_cmd)
        self.assertIn(remote_dir, push_cmd)
        self.assertNotIn("--sync", push_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_push_list(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        files = ["myfile", "file2"]
        remote_dir = "/remotedir"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.push(files, remote_dir)
        push_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        self.assertIn(" ".join(files), push_cmd)
        self.assertIn(remote_dir, push_cmd)
        self.assertNotIn("--sync", push_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_push_sync(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        files = ["myfile", "file2"]
        remote_dir = "/remotedir"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.push(files, remote_dir, sync=True)
        push_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        for file_name in files:
            self.assertIn(file_name, push_cmd)
        self.assertIn(remote_dir, push_cmd)
        self.assertIn("--sync", push_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_pull(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        files = "myfile"
        local_dir = "/localdir"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.pull(files, local_dir)
        pull_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        self.assertIn(files, pull_cmd)
        self.assertIn(local_dir, pull_cmd)
        self.assertNotIn("--sync", pull_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_pull_files(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        files = ["myfile", "file2"]
        local_dir = "/localdir"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.pull(files, local_dir)
        pull_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        for file_name in files:
            self.assertIn(file_name, pull_cmd)
        self.assertIn(local_dir, pull_cmd)
        self.assertNotIn("--sync", pull_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test__battery_cmd(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        option = "myopt"
        battery_cmd = "shell dumpsys batterystats " + option
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb._battery_cmd(option)
        mock_runcmd.RunCommand.assert_called_once_with("adb " + battery_cmd, unittest.mock.ANY,
                                                       unittest.mock.ANY)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test__battery_cmd_save_output(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        option = "myopt"
        battery_cmd = "shell dumpsys batterystats " + option
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb._battery_cmd(option, "myfile")
        mock_runcmd.RunCommand.assert_called_once_with("adb " + battery_cmd, unittest.mock.ANY,
                                                       unittest.mock.ANY)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_battery(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.battery("myfile")
        battery_cmd = mock_runcmd.RunCommand.call_args_list[0][0][0]
        self.assertIn("--checkin", battery_cmd)
        self.assertNotIn("--reset", battery_cmd)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_battery_reset(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.battery("myfile", reset=True, append_file=True)
        battery_cmd1 = mock_runcmd.RunCommand.call_args_list[0][0][0]
        battery_cmd2 = mock_runcmd.RunCommand.call_args_list[1][0][0]
        self.assertIn("--reset", battery_cmd1)
        self.assertIn("--checkin", battery_cmd2)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_memory(self, mock_runcmd, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        memory_cmd = "shell dumpsys meminfo -c -d"
        mock_os.path.dirname.return_value = "adb_dir"
        adb = adb_monitor.Adb()
        adb.memory("myfile")
        mock_runcmd.RunCommand.assert_called_once_with("adb " + memory_cmd, unittest.mock.ANY,
                                                       unittest.mock.ANY)

    @patch(ns("tempfile.NamedTemporaryFile"))
    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_systrace_start(self, mock_runcmd, mock_find_executable, mock_os, mock_tempfile):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        systrace_script = "systrace.py"
        mock_os.path.join.return_value = systrace_script
        adb = adb_monitor.Adb()
        adb.systrace_start()
        mock_runcmd.RunCommand.assert_called_once_with(output_file=mock_tempfile().name,
                                                       propagate_signals=False)
        self.assertEqual(adb.systrace_script, systrace_script)
        self.assertEqual(mock_runcmd.RunCommand().add_file.call_count, 3)
        mock_runcmd.RunCommand().add_file.assert_any_call(adb_monitor.DEFAULT_PYTHON27)
        mock_runcmd.RunCommand().add_file.assert_any_call(systrace_script)
        self.assertEqual(mock_runcmd.RunCommand().add.call_count, 3)
        mock_runcmd.RunCommand().add.assert_any_call("--json")
        mock_runcmd.RunCommand().add.assert_any_call("-o")
        mock_runcmd.RunCommand().add.assert_any_call("dalvik sched freq idle load")
        mock_runcmd.RunCommand().start_process.assert_called_once()

    @patch(ns("tempfile.NamedTemporaryFile"))
    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    @patch(ns("runcommand"))
    def test_systrace_start_output_file(self, mock_runcmd, mock_find_executable, mock_os,
                                        mock_tempfile):
        # pylint: disable=unused-argument
        output_file = "myfile"
        mock_os.path.dirname.return_value = "adb_dir"
        systrace_script = "systrace.py"
        mock_os.path.join.return_value = systrace_script
        adb = adb_monitor.Adb()
        adb.systrace_start(output_file)
        mock_runcmd.RunCommand.assert_called_once_with(output_file=mock_tempfile().name,
                                                       propagate_signals=False)
        self.assertEqual(adb.systrace_script, systrace_script)
        self.assertEqual(mock_runcmd.RunCommand().add_file.call_count, 3)
        mock_runcmd.RunCommand().add_file.assert_any_call(adb_monitor.DEFAULT_PYTHON27)
        mock_runcmd.RunCommand().add_file.assert_any_call(systrace_script)
        mock_runcmd.RunCommand().add_file.assert_any_call(output_file)
        self.assertEqual(mock_runcmd.RunCommand().add.call_count, 3)
        mock_runcmd.RunCommand().add.assert_any_call("--json")
        mock_runcmd.RunCommand().add.assert_any_call("-o")
        mock_runcmd.RunCommand().add.assert_any_call("dalvik sched freq idle load")
        mock_runcmd.RunCommand().start_process.assert_called_once()

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test_systrace_stop(self, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        systrace_script = "systrace.py"
        systrace_output = "Systrace: Wrote trace"
        mock_os.path.join.return_value = systrace_script
        adb = adb_monitor.Adb()
        adb._cmd = MagicMock()
        with patch(ADB_MONITOR + ".open", mock_open(read_data=systrace_output)):
            adb.systrace_stop()
        adb._cmd.send_to_process.assert_called_once_with(b"bye")
        mock_os.remove.assert_called_once_with(adb._tempfile)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test_systrace_stop_output_file(self, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        systrace_script = "systrace.py"
        systrace_output = "Systrace: Wrote trace"
        output_file = "myfile"
        mock_os.path.join.return_value = systrace_script
        adb = adb_monitor.Adb()
        adb._cmd = MagicMock()
        with patch(ADB_MONITOR + ".open", mock_open(read_data=systrace_output)):
            adb.systrace_stop(output_file=output_file)
        adb._cmd.send_to_process.assert_called_once_with(b"bye")
        mock_os.remove.assert_called_once_with(adb._tempfile)

    @patch(ns("os"))
    @patch(ns("find_executable"), side_effect=lambda x: x)
    def test_systrace_stop_no_trace(self, mock_find_executable, mock_os):
        # pylint: disable=unused-argument
        mock_os.path.dirname.return_value = "adb_dir"
        systrace_script = "systrace.py"
        systrace_output = "Systrace: did not Write trace"
        output_file = "myfile"
        mock_os.path.join.return_value = systrace_script
        adb = adb_monitor.Adb()
        adb._cmd = MagicMock()
        with patch(ADB_MONITOR + ".open", mock_open(read_data=systrace_output)):
            adb.systrace_stop(output_file=output_file)
        adb._cmd.send_to_process.assert_called_once_with(b"bye")
        self.assertEqual(mock_os.remove.call_count, 2)
        mock_os.remove.assert_any_call(adb._tempfile)
        mock_os.remove.assert_any_call(output_file)


class TestAdbControl(unittest.TestCase):
    def test___init___no_files(self):
        mock_adb = MagicMock()
        with self.assertRaises(ValueError):
            adb_monitor.AdbControl(mock_adb)

    @patch(ns("fileops.create_empty"))
    def test___init___all_files(self, mock_create_empty):
        mock_adb = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        adb_control = adb_monitor.AdbControl(mock_adb, battery_file=battery_file,
                                             memory_file=memory_file, cpu_file=cpu_file)
        self.assertEqual(adb_control.adb, mock_adb)
        self.assertEqual(adb_control.battery_file, battery_file)
        self.assertEqual(adb_control.memory_file, memory_file)
        self.assertEqual(adb_control.cpu_file, cpu_file)
        self.assertEqual(adb_control.num_samples, 0)
        self.assertIsNone(adb_control.collection_time_secs)
        self.assertEqual(adb_control.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)
        self.assertEqual(mock_create_empty.call_count, 3)
        mock_create_empty.assert_any_call(battery_file)
        mock_create_empty.assert_any_call(memory_file)
        mock_create_empty.assert_any_call(cpu_file)

    @patch(ns("fileops.create_empty"))
    def test___init___battery_file(self, mock_create_empty):
        mock_adb = MagicMock()
        battery_file = "mybattery"
        adb_control = adb_monitor.AdbControl(mock_adb, battery_file=battery_file)
        self.assertEqual(adb_control.adb, mock_adb)
        self.assertEqual(adb_control.battery_file, battery_file)
        self.assertIsNone(adb_control.memory_file)
        self.assertIsNone(adb_control.cpu_file)
        self.assertEqual(adb_control.num_samples, 0)
        self.assertIsNone(adb_control.collection_time_secs)
        self.assertEqual(adb_control.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)
        mock_create_empty.assert_called_once_with(battery_file)

    @patch(ns("fileops.create_empty"))
    def test___init___all_params(self, mock_create_empty):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        num_samples = 5
        collection_time_secs = 10
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            cpu_file=cpu_file, append_file=True, num_samples=num_samples, collection_time_secs=10,
            sample_interval_ms=sample_interval_ms)
        self.assertEqual(adb_control.adb, mock_adb)
        self.assertEqual(adb_control.battery_file, battery_file)
        self.assertEqual(adb_control.memory_file, memory_file)
        self.assertEqual(adb_control.cpu_file, cpu_file)
        self.assertEqual(adb_control.num_samples, 0)
        self.assertEqual(adb_control.collection_time_secs, collection_time_secs)
        self.assertEqual(adb_control.sample_interval_ms, sample_interval_ms)
        mock_create_empty.assert_not_called()

    @patch(ns("fileops.create_empty"))
    def test___init___num_samples(self, mock_create_empty):
        # pylint: disable=unused-argument
        mock_adb = MagicMock()
        battery_file = "mybattery"
        num_samples = 5
        adb_control = adb_monitor.AdbControl(mock_adb, battery_file=battery_file,
                                             num_samples=num_samples)
        self.assertEqual(adb_control.adb, mock_adb)
        self.assertEqual(adb_control.battery_file, battery_file)
        self.assertIsNone(adb_control.memory_file)
        self.assertIsNone(adb_control.cpu_file)
        self.assertEqual(adb_control.num_samples, num_samples)
        self.assertIsNone(adb_control.collection_time_secs)
        self.assertEqual(adb_control.sample_interval_ms, adb_monitor.DEFAULT_SAMPLE_INTERVAL_MS)

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_start_all(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        num_samples = 5
        collection_time_secs = 10
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            cpu_file=cpu_file, append_file=True, num_samples=num_samples,
            collection_time_secs=collection_time_secs, sample_interval_ms=sample_interval_ms)
        adb_control.start()
        self.assertEqual(len(adb_control._all_threads), 3)
        self.assertEqual(len(adb_control._sample_based_threads), 2)
        mock_continuous_monitor.assert_called_once_with(
            cpu_file, adb_control._should_stop, mock_adb.systrace_start, mock_adb.systrace_stop)
        mock_continuous_monitor().start.assert_called_once()
        self.assertEqual(mock_sample_monitor.call_count, 2)
        mock_sample_monitor.assert_any_call(battery_file, adb_control._should_stop,
                                            mock_adb.battery, 0, sample_interval_ms)
        mock_sample_monitor.assert_any_call(memory_file, adb_control._should_stop, mock_adb.memory,
                                            0, sample_interval_ms)
        self.assertEqual(mock_sample_monitor().start.call_count, 2)

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_start_cpu(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        cpu_file = "mycpu"
        collection_time_secs = 10
        adb_control = adb_monitor.AdbControl(mock_adb, logger=mock_logger, cpu_file=cpu_file,
                                             append_file=True,
                                             collection_time_secs=collection_time_secs)
        adb_control.start()
        self.assertEqual(len(adb_control._all_threads), 1)
        self.assertEqual(len(adb_control._sample_based_threads), 0)
        mock_continuous_monitor.assert_called_once_with(
            cpu_file, adb_control._should_stop, mock_adb.systrace_start, mock_adb.systrace_stop)
        mock_continuous_monitor().start.assert_called_once()
        mock_sample_monitor.assert_not_called()

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_start_battery(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        num_samples = 5
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, append_file=True,
            num_samples=num_samples, sample_interval_ms=sample_interval_ms)
        adb_control.start()
        self.assertEqual(len(adb_control._all_threads), 1)
        self.assertEqual(len(adb_control._sample_based_threads), 1)
        mock_continuous_monitor.assert_not_called()
        mock_sample_monitor.assert_called_once_with(battery_file, adb_control._should_stop,
                                                    mock_adb.battery, num_samples,
                                                    sample_interval_ms)
        mock_sample_monitor().start.assert_called_once()

    def test_stop(self):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        num_samples = 5
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, append_file=True,
            num_samples=num_samples, sample_interval_ms=sample_interval_ms)
        adb_control._should_stop = MagicMock()
        adb_control.wait = MagicMock()
        adb_control.stop()
        adb_control._should_stop.set.assert_called_once()
        adb_control.wait.assert_called_once()

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_wait(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        num_samples = 5
        collection_time_secs = 10
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            cpu_file=cpu_file, append_file=True, num_samples=num_samples, collection_time_secs=10,
            sample_interval_ms=sample_interval_ms)
        mock_sample_monitor().exception = None
        mock_continuous_monitor().exception = None
        adb_control._should_stop = MagicMock()
        adb_control.start()
        adb_control.wait()
        adb_control._should_stop.wait.assert_called_once_with(collection_time_secs)
        adb_control._should_stop.set.assert_called_once()
        mock_continuous_monitor().join.assert_called_once_with(adb_control._JOIN_TIMEOUT)
        self.assertEqual(mock_sample_monitor().join.call_count, 2)
        mock_sample_monitor().join.assert_called_with(adb_control._JOIN_TIMEOUT)

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_wait_keyboard_interrupt(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        num_samples = 5
        collection_time_secs = 10
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            cpu_file=cpu_file, append_file=True, num_samples=num_samples, collection_time_secs=10,
            sample_interval_ms=sample_interval_ms)
        mock_sample_monitor().exception = None
        mock_continuous_monitor().exception = None
        adb_control._should_stop = MagicMock()
        adb_control._should_stop.wait.side_effect = KeyboardInterrupt()
        adb_control.start()
        adb_control.wait()
        adb_control._should_stop.wait.assert_called_once_with(collection_time_secs)
        adb_control._should_stop.set.assert_called_once()
        mock_continuous_monitor().join.assert_called_once_with(adb_control._JOIN_TIMEOUT)
        self.assertEqual(mock_sample_monitor().join.call_count, 2)
        mock_sample_monitor().join.assert_called_with(adb_control._JOIN_TIMEOUT)

    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_wait_no_collection_time(self, mock_sample_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        num_samples = 5
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            append_file=True, num_samples=num_samples, sample_interval_ms=sample_interval_ms)
        mock_sample_monitor().exception = None
        adb_control._should_stop = MagicMock()
        adb_control.start()
        adb_control.wait()
        adb_control._should_stop.wait.assert_not_called()
        adb_control._should_stop.set.assert_called_once()
        self.assertEqual(mock_sample_monitor().join.call_count, 4)
        mock_sample_monitor().join.assert_called_with(adb_control._JOIN_TIMEOUT)

    @patch(ns("AdbContinuousResourceMonitor"))
    @patch(ns("AdbSampleBasedResourceMonitor"))
    def test_wait_thread_exception(self, mock_sample_monitor, mock_continuous_monitor):
        mock_adb = MagicMock()
        mock_logger = MagicMock()
        battery_file = "mybattery"
        memory_file = "mymemory"
        cpu_file = "mycpu"
        num_samples = 5
        collection_time_secs = 10
        sample_interval_ms = 25
        adb_control = adb_monitor.AdbControl(
            mock_adb, logger=mock_logger, battery_file=battery_file, memory_file=memory_file,
            cpu_file=cpu_file, append_file=True, num_samples=num_samples, collection_time_secs=10,
            sample_interval_ms=sample_interval_ms)
        mock_sample_monitor().exception = RuntimeError("my run time exception")
        mock_continuous_monitor().exception = None
        adb_control._should_stop = MagicMock()
        adb_control.start()
        with self.assertRaises(RuntimeError):
            adb_control.wait()
        adb_control._should_stop.wait.assert_called_once_with(collection_time_secs)
        adb_control._should_stop.set.assert_called_once()
        mock_continuous_monitor().join.assert_called_once_with(adb_control._JOIN_TIMEOUT)
        self.assertEqual(mock_sample_monitor().join.call_count, 2)
        mock_sample_monitor().join.assert_called_with(adb_control._JOIN_TIMEOUT)


class TestAdbResourceMonitor(unittest.TestCase):
    @patch(ns("threading"))
    def test_run(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        should_stop = MagicMock()
        arm = adb_monitor.AdbResourceMonitor(output_file, should_stop)
        arm._do_monitoring = MagicMock()
        arm.run()
        self.assertEqual(arm._output_file, output_file)
        arm._do_monitoring.assert_called_once()
        self.assertIsNone(arm.exception)
        should_stop.set.assert_not_called()

    @patch(ns("threading"))
    def test_run_exception(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        should_stop = MagicMock()
        exception = RuntimeError()
        arm = adb_monitor.AdbResourceMonitor(output_file, should_stop)
        arm._do_monitoring = MagicMock(side_effect=exception)
        arm.run()
        self.assertEqual(arm._output_file, output_file)
        arm._do_monitoring.assert_called_once()
        self.assertEqual(arm.exception, exception)
        should_stop.set.assert_called_once()


class TestAdbSampleBasedResourceMonitor(unittest.TestCase):
    @patch(ns("threading"))
    def test__do_monitoring(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        mock_should_stop = MagicMock()
        mock_should_stop.is_set.return_value = False
        mock_adb_cmd = MagicMock()
        num_samples = 5
        sample_interval_ms = 50
        arm = adb_monitor.AdbSampleBasedResourceMonitor(output_file, mock_should_stop, mock_adb_cmd,
                                                        num_samples, sample_interval_ms)
        arm._take_sample = MagicMock()
        arm._do_monitoring()
        self.assertEqual(mock_should_stop.is_set.call_count, num_samples + 1)
        self.assertEqual(arm._take_sample.call_count, num_samples)
        self.assertEqual(mock_should_stop.wait.call_count, num_samples - 1)
        mock_should_stop.wait.assert_called_with(sample_interval_ms / 1000.0)

    def _is_set(self):
        self.num_collected += 1
        return self.num_collected == self.num_samples

    @patch(ns("threading"))
    def test__do_monitoring_no_samples(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        mock_should_stop = MagicMock()
        self.num_collected = 0  # pylint: disable=attribute-defined-outside-init
        self.num_samples = 5  # pylint: disable=attribute-defined-outside-init
        mock_should_stop.is_set = self._is_set
        mock_adb_cmd = MagicMock()
        num_samples = 0
        sample_interval_ms = 50
        arm = adb_monitor.AdbSampleBasedResourceMonitor(output_file, mock_should_stop, mock_adb_cmd,
                                                        num_samples, sample_interval_ms)
        arm._take_sample = MagicMock()
        arm._do_monitoring()
        self.assertEqual(arm._take_sample.call_count, self.num_samples - 1)
        self.assertEqual(mock_should_stop.wait.call_count, self.num_samples - 2)

    @patch(ns("threading"))
    def test__take_sample(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        mock_should_stop = MagicMock()
        self.num_collected = 0  # pylint: disable=attribute-defined-outside-init
        self.num_samples = 5  # pylint: disable=attribute-defined-outside-init
        mock_should_stop.is_set = self._is_set
        mock_adb_cmd = MagicMock()
        arm = adb_monitor.AdbSampleBasedResourceMonitor(output_file, mock_should_stop, mock_adb_cmd,
                                                        10, 20)
        arm._take_sample(2)
        mock_adb_cmd.assert_called_once_with(output_file=output_file, append_file=True)


class TestAdbContinuousResourceMonitor(unittest.TestCase):
    @patch(ns("threading"))
    def test__do_monitoring(self, mock_threading):
        # pylint: disable=unused-argument
        output_file = "myfile"
        mock_should_stop = MagicMock()
        mock_adb_start_cmd = MagicMock()
        mock_adb_stop_cmd = MagicMock()
        acrm = adb_monitor.AdbContinuousResourceMonitor(output_file, mock_should_stop,
                                                        mock_adb_start_cmd, mock_adb_stop_cmd)
        acrm._do_monitoring()
        mock_should_stop.wait.assert_called_once()
        mock_adb_start_cmd.assert_called_with(output_file=output_file)
        mock_adb_stop_cmd.assert_called_with(output_file=output_file)
