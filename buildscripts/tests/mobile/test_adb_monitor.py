""" Unit tests for adb_monitor. """

import distutils.spawn  # pylint: disable=no-name-in-module
import os
import shutil
import sys
import tempfile
import unittest

import buildscripts.mobile.adb_monitor as adb_monitor

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"

if _IS_WINDOWS:
    import win32file

# pylint: disable=missing-docstring,protected-access


def mock_adb_and_systrace(directory):
    """Mock adb and systrace.py."""
    # Create mock 'adb', which is really 'echo'.
    adb_binary = os.path.join(directory, "adb")
    echo_binary = distutils.spawn.find_executable("echo")
    if _IS_WINDOWS:
        adb_binary = "{}.exe".format(adb_binary)
        shutil.copyfile(echo_binary, adb_binary)
    else:
        os.symlink(echo_binary, adb_binary)
    os.environ["PATH"] = "{}{}{}".format(directory, os.path.pathsep, os.environ["PATH"])

    # Create mock 'systrace.py'.
    systrace_dir = os.path.join(directory, "systrace")
    os.mkdir(systrace_dir)
    systrace = os.path.join(systrace_dir, "systrace.py")
    with open(systrace, "w") as fh:
        fh.write("import optparse\n")
        fh.write("input('waiting...')\n")
        fh.write("print('Wrote trace')\n")
        fh.write("parser = optparse.OptionParser()\n")
        fh.write("parser.add_option('-o', dest='output_file')\n")
        fh.write("parser.add_option('--json', dest='json_opts', action='store_true')\n")
        fh.write("options, args = parser.parse_args()\n")
        fh.write("with open(options.output_file, 'w') as fh:\n")
        fh.write("\tfh.write('{hello:1}')\n")


def file_contents(path):
    with open(path, "r") as fh:
        return fh.read()


class AdbTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.mkdtemp()
        mock_adb_and_systrace(cls.temp_dir)
        cls.adb = adb_monitor.Adb()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.temp_dir)


class AdbTest(AdbTestCase):
    def test_bad_adb(self):
        self.assertRaises(EnvironmentError, lambda: adb_monitor.Adb("bad_adb"))

    def test_devices(self):
        self.adb.devices()

    def test_battery(self):
        temp_file = os.path.join(self.temp_dir, "battery_output")
        self.adb.battery(output_file=temp_file)
        self.assertTrue(os.path.isfile(temp_file))

    def test_memory(self):
        temp_file = os.path.join(self.temp_dir, "memory_output")
        self.adb.memory(output_file=temp_file)
        self.assertTrue(os.path.isfile(temp_file))

    def test_systrace(self):
        temp_file = os.path.join(self.temp_dir, "systrace_output")
        self.adb.systrace_start(output_file=temp_file)
        self.adb.systrace_stop()
        self.assertTrue(os.path.isfile(temp_file))


class AdbControlTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.mkdtemp()
        mock_adb_and_systrace(cls.temp_dir)
        cls.adb = adb_monitor.Adb()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.temp_dir)


class AdbControlTest(AdbControlTestCase):
    def _test_files(self, num_samples=1, collection_time_secs=0, sample_interval_ms=500,
                    arg_list=None):
        args = {}
        arg_file_list = []
        for arg_name in arg_list:
            arg_test_file = tempfile.NamedTemporaryFile(dir=self.temp_dir, delete=False).name
            args[arg_name] = arg_test_file
            arg_file_list.append(arg_test_file)
        adb_control = adb_monitor.AdbControl(self.adb, collection_time_secs=collection_time_secs,
                                             num_samples=num_samples,
                                             sample_interval_ms=sample_interval_ms, **args)
        adb_control.start()
        adb_control.wait()
        for arg_file in arg_file_list:
            self.assertGreater(os.stat(arg_file).st_size, 0)
            os.remove(arg_file)

    def test_all_files_num_samples(self):
        self._test_files(num_samples=5, arg_list=["battery_file", "cpu_file", "memory_file"])

    def test_all_files_collection_time_secs(self):
        self._test_files(collection_time_secs=3,
                         arg_list=["battery_file", "cpu_file", "memory_file"])

    def test_all_files_collection_and_samples(self):
        self._test_files(collection_time_secs=3, num_samples=5,
                         arg_list=["battery_file", "cpu_file", "memory_file"])

    def test_no_file_arg(self):
        self.assertRaises(ValueError, lambda: adb_monitor.AdbControl(self.adb))

    def test_bad_file_arg(self):
        self.assertRaises(TypeError, lambda: self._test_files(arg_list=["bad_file_arg"]))
