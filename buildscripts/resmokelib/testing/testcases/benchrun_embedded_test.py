"""The unittest.TestCase for tests using benchrun embedded (mongoebench)."""

import os
import posixpath

from buildscripts.mobile import adb_monitor
from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import parser
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface


class BenchrunEmbeddedTestCase(  # pylint: disable=too-many-instance-attributes
        interface.ProcessTestCase):
    """A Benchrun embedded test to execute."""

    REGISTERED_NAME = "benchrun_embedded_test"

    def __init__(self, logger, mongoebench_config_file, program_options=None):
        """Initialize the BenchrunEmbeddedTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "Benchmark embedded test",
                                           mongoebench_config_file)
        parser.validate_benchmark_options()

        self.benchrun_config_file = mongoebench_config_file

        # Command line options override the YAML configuration.
        self.benchrun_executable = utils.default_if_none(_config.MONGOEBENCH_EXECUTABLE,
                                                         _config.DEFAULT_MONGOEBENCH_EXECUTABLE)
        self.benchrun_repetitions = utils.default_if_none(_config.BENCHMARK_REPETITIONS,
                                                          _config.DEFAULT_BENCHMARK_REPETITIONS)
        self.suite_benchrun_options = program_options
        self.benchrun_threads = 1
        if program_options and "threads" in program_options:
            self.benchrun_threads = program_options["threads"]
        self.report_root = _config.BENCHRUN_REPORT_ROOT
        self.benchrun_options = {}

        # Set the dbpath.
        dbpath = utils.default_if_none(_config.DBPATH_PREFIX, _config.DEFAULT_DBPATH_PREFIX)
        self.dbpath = os.path.join(dbpath, "mongoebench")

        self.android_device = _config.BENCHRUN_DEVICE == "Android"
        # If Android device, then the test runs via adb shell.
        if self.android_device:
            self.adb = adb_monitor.Adb()
            self.android_benchrun_root = _config.BENCHRUN_EMBEDDED_ROOT
            self.device_report_root = posixpath.join(self.android_benchrun_root, "results")
            self.dbpath = posixpath.join(self.android_benchrun_root, "db")
            self.benchrun_config_file = posixpath.join(self.android_benchrun_root, "testcases",
                                                       os.path.basename(self.benchrun_config_file))
            ld_library_path = "LD_LIBRARY_PATH={}".format(
                posixpath.join(self.android_benchrun_root, "sdk"))
            mongoebench = posixpath.join(self.android_benchrun_root, "sdk", "mongoebench")
            self.benchrun_executable = "adb shell {} {}".format(ld_library_path, mongoebench)

    def configure(self, fixture, *args, **kwargs):
        """Configure BenchrunEmbeddedTestCase."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        # 1. Set the default benchmark options.
        benchrun_options = {"time": _config.DEFAULT_BENCHMARK_MIN_TIME.total_seconds()}

        # 2. Override Benchmark options with options set through `program_options` in the suite
        #    configuration.
        suite_benchrun_options = utils.default_if_none(self.suite_benchrun_options, {})
        benchrun_options.update(suite_benchrun_options)

        # 3. Override Benchmark options with options set through resmoke's command line.
        resmoke_benchrun_options = {"dbpath": self.dbpath, "time": _config.BENCHMARK_MIN_TIME}

        for key, value in list(resmoke_benchrun_options.items()):
            if value is not None:
                # 4. sanitize options before passing them to Benchmark's command line.
                if key == "time":
                    value = value.total_seconds()
                benchrun_options[key] = value

        self.benchrun_options = benchrun_options

        # Create the test report directory.
        utils.rmtree(self._report_dir(), ignore_errors=True)
        try:
            os.makedirs(self._report_dir())
        except os.error:
            # Directory already exists.
            pass

        # Create the dbpath.
        if self.android_device:
            self.adb.shell("rm -fr {}".format(self.dbpath))
            self.adb.shell("mkdir {}".format(self.dbpath))
        else:
            utils.rmtree(self.dbpath, ignore_errors=True)
            try:
                os.makedirs(self.dbpath)
            except os.error:
                # Directory already exists.
                pass

    def run_test(self):
        """Run the test for specified number of iterations."""
        for iter_num in range(self.benchrun_repetitions):
            # Set the output file for each iteration.
            local_report_path = self._report_path(iter_num)
            device_report_path = self._device_report_path(iter_num)
            self.benchrun_options["output"] = device_report_path
            interface.ProcessTestCase.run_test(self)
            self._move_report(device_report_path, local_report_path)

    def _move_report(self, remote_path, local_path):
        """Move report from device to local directory."""
        if self.android_device:
            # Pull test result from the Android device and then delete it from the device.
            self.logger.info("Moving report %s from device to local %s ...", remote_path,
                             local_path)
            self.adb.pull(remote_path, local_path)
            self.adb.shell("rm {}".format(remote_path))

    def _device_report_path(self, iter_num):
        """Return the device report path."""
        if self.android_device:
            # The mongoebench report is generated on the remote device.
            return posixpath.join(self.device_report_root, self._report_name(iter_num))
        return self._report_path(iter_num)

    def _report_path(self, iter_num):
        """Return the local report path."""
        return os.path.join(self._report_dir(), self._report_name(iter_num))

    def _report_dir(self):
        """Return the report directory. Reports are stored in <report_root>/<testname>/<thread>."""
        return os.path.join(self.report_root, self.short_name(),
                            "thread{}".format(self.benchrun_threads))

    @staticmethod
    def _report_name(iter_num):
        """Return the constructed report name of the form mongoebench.<iteration num>.json."""
        return "mongoebench.{}.json".format(iter_num)

    def _make_process(self):
        # The 'commands' argument for core.programs.generic_program must be a list.
        commands = self.benchrun_executable.split()
        commands.append(self.benchrun_config_file)
        return core.programs.generic_program(self.logger, commands, **self.benchrun_options)
