"""Unit tests for the buildscripts.resmokelib.logging.buildlogger module."""

import json
import unittest

from buildscripts.resmokelib.logging import buildlogger

# pylint: disable=protected-access


class TestLogsSplitter(unittest.TestCase):
    """Unit tests for the _LogsSplitter class."""

    def test_split_no_logs(self):
        logs = []
        max_size = 10
        self.assertEqual([[]], buildlogger._LogsSplitter.split_logs(logs, max_size))

    def test_split_no_max_size(self):
        logs = self.__generate_logs(size=30)
        max_size = None
        self.assertEqual([logs], buildlogger._LogsSplitter.split_logs(logs, max_size))

    def test_split_max_size_smaller(self):
        logs = self.__generate_logs(size=20)
        max_size = 30
        self.assertEqual([logs], buildlogger._LogsSplitter.split_logs(logs, max_size))

    def test_split_max_size_equal(self):
        logs = self.__generate_logs(size=30)
        max_size = 30
        self.assertEqual([logs], buildlogger._LogsSplitter.split_logs(logs, max_size))

    def test_split_max_size_larger(self):
        logs = self.__generate_logs(size=31)
        max_size = 30
        self.assertEqual([logs[0:-1], logs[-1:]],
                         buildlogger._LogsSplitter.split_logs(logs, max_size))

        logs = self.__generate_logs(size=149)
        max_size = 19
        self.assertEqual([
            logs[0:3], logs[3:6], logs[6:9], logs[9:12], logs[12:15], logs[15:18], logs[18:21],
            logs[21:24], logs[24:27], logs[27:]
        ], buildlogger._LogsSplitter.split_logs(logs, max_size))

    def check_split_sizes(self, splits, max_size):
        for split in splits:
            self.assertTrue(TestLogsSplitter.size(split) <= max_size)

    def __generate_logs(self, size):
        # The size of [ "x" ] is 5. This is the minimum size we generate.
        self.assertTrue(size >= 5)
        # Each new "x" adds 5 to the size.
        nb_lines = int(size / 5)
        # Each additional "x" on a line adds 1 to the size.
        last_line_extra = size % 5
        logs = ["x"] * nb_lines
        logs[-1] += "x" * last_line_extra
        self.assertEqual(size, TestLogsSplitter.size(logs))
        return logs

    @staticmethod
    def size(logs):
        """Returns the size of the log lines when represented in JSON."""
        return len(json.dumps(logs))
