"""Unit tests for the buildscripts.resmokelib.hang_analyzer.process_list module."""

import unittest

from mock import Mock, patch

from buildscripts.resmokelib.hang_analyzer.process_list import Pinfo, get_processes

NS = "buildscripts.resmokelib.hang_analyzer.process_list"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


class TestGetProcesses(unittest.TestCase):
    """Unit tests for the get_processes method."""

    @patch(ns("os.getpid"))
    @patch(ns("_get_lister"))
    def test_interesting_processes(self, lister_mock, os_mock):
        os_mock.return_value = -1
        lister_mock.return_value.dump_processes.return_value = [
            (1, "python"),
            (2, "mongo"),
            (3, "python"),
            (4, "mongod"),
            (5, "java")  # this should be ignored.
        ]

        process_ids = None
        interesting_processes = ['python', 'mongo', 'mongod']
        process_match = "exact"
        logger = Mock()

        processes = get_processes(process_ids, interesting_processes, process_match, logger)

        self.assertCountEqual(processes, [
            Pinfo(name="python", pidv=[1, 3]),
            Pinfo(name="mongo", pidv=[2]),
            Pinfo(name="mongod", pidv=[4])
        ])

    @patch(ns("os.getpid"))
    @patch(ns("_get_lister"))
    def test_interesting_processes_and_process_ids(self, lister_mock, os_mock):
        os_mock.return_value = -1
        lister_mock.return_value.dump_processes.return_value = [(1, "python"), (2, "mongo"),
                                                                (3, "python"), (4, "mongod"),
                                                                (5, "java")]

        process_ids = [1, 2, 5]
        interesting_processes = ['python', 'mongo', 'mongod']
        process_match = "exact"
        logger = Mock()

        processes = get_processes(process_ids, interesting_processes, process_match, logger)

        self.assertCountEqual(processes, [
            Pinfo(name="python", pidv=[1]),
            Pinfo(name="mongo", pidv=[2]),
            Pinfo(name="java", pidv=[5]),
        ])

    @patch(ns("os.getpid"))
    @patch(ns("_get_lister"))
    def test_interesting_processes_contains(self, lister_mock, os_mock):
        os_mock.return_value = -1
        lister_mock.return_value.dump_processes.return_value = [
            (1, "python2"),
            (2, "mongo"),
            (3, "python3"),
            (4, "mongod"),
            (5, "python"),
            (5, "java")  # this should be ignored.
        ]

        process_ids = None
        interesting_processes = ['python', 'mongo', 'mongod']
        process_match = "contains"
        logger = Mock()

        processes = get_processes(process_ids, interesting_processes, process_match, logger)

        self.assertCountEqual(processes, [
            Pinfo(name="python", pidv=[5]),
            Pinfo(name="python2", pidv=[1]),
            Pinfo(name="python3", pidv=[3]),
            Pinfo(name="mongo", pidv=[2]),
            Pinfo(name="mongod", pidv=[4])
        ])

    @patch(ns("os.getpid"))
    @patch(ns("_get_lister"))
    def test_process_ids(self, lister_mock, os_mock):
        os_mock.return_value = -1
        lister_mock.return_value.dump_processes.return_value = [
            (1, "python"),
            (2, "mongo"),
            (3, "python"),
            (4, "mongod"),
            (5, "mongod"),
            (6, "python"),  # rest is ignored
            (7, "mongod"),
            (8, "mongo"),
            (9, "java"),
        ]

        process_ids = [1, 2, 3, 4, 5]
        interesting_processes = []
        process_match = "exact"
        logger = Mock()

        processes = get_processes(process_ids, interesting_processes, process_match, logger)

        self.assertCountEqual(processes, [
            Pinfo(name="python", pidv=[1, 3]),
            Pinfo(name="mongo", pidv=[2]),
            Pinfo(name="mongod", pidv=[4, 5])
        ])
