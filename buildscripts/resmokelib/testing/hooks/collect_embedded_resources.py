"""Module for generating and collecting embedded resource results."""

import os

from buildscripts.mobile import adb_monitor
from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.testing.hooks import interface


class CollectEmbeddedResources(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """CollectEmbeddedResources class.

    CollectEmbeddedResources starts and stops the resource monitoring for each test.
    """

    DESCRIPTION = "Embedded resources"

    def __init__(self, hook_logger, fixture, sample_interval_ms=500, threads=1):
        """Initialize CollectEmbeddedResources."""
        interface.Hook.__init__(self, hook_logger, fixture, CollectEmbeddedResources.DESCRIPTION)
        self.hook_logger = hook_logger
        self.adb = None
        self.adb_control = None
        if _config.BENCHRUN_DEVICE == "Android":
            self.report_root = _config.BENCHRUN_REPORT_ROOT
            self.sample_interval_ms = sample_interval_ms
            self.threads = threads
            self.battery_file = "battery.csv"
            self.cpu_file = "cpu.json"
            self.memory_file = "memory.csv"
            self.adb = adb_monitor.Adb(logger=hook_logger)

    def before_test(self, test, test_report):
        """Start ADB monitoring."""
        if self.adb:
            battery_file = self._report_path(test, "battery.csv")
            cpu_file = self._report_path(test, "cpu.json")
            memory_file = self._report_path(test, "memory.csv")
            self.adb_control = adb_monitor.AdbControl(
                self.adb, logger=self.hook_logger, battery_file=battery_file, cpu_file=cpu_file,
                memory_file=memory_file, sample_interval_ms=self.sample_interval_ms)
            self.hook_logger.info("Starting ADB monitoring for test %s", test.short_name())
            self.hook_logger.info("ADB resource files: %s %s %s", battery_file, cpu_file,
                                  memory_file)
            self.adb_control.start()

    def after_test(self, test, test_report):
        """Stop ADB monitoring."""
        if self.adb_control:
            self.hook_logger.info("Stopping ADB monitoring for test %s", test.short_name())
            self.adb_control.stop()

    def _report_path(self, test, report_name):
        """Return the report path. Reports are stored in <report_root>/<testname>/thread<num>/."""
        return os.path.join(self.report_root, test.short_name(), "thread{}".format(self.threads),
                            report_name)
