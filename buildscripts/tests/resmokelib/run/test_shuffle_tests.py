import random
import unittest
from collections import namedtuple

from buildscripts.resmokelib.run import TestRunner
from buildscripts.util.teststats import HistoricalTestInformation, HistoricTaskData


class TestShuffle(unittest.TestCase):
    def test_random_shuffle(self):
        random.seed(0)
        tests = ["a", "b", "c", "d"]
        expected = ["c", "a", "b", "d"]
        actual = TestRunner.RandomShuffle().shuffle(tests)
        self.assertListEqual(actual, expected)

    def test_slowest_first_partial_shuffle(self):
        tests = ["a", "b", "c", "d"]
        history = HistoricTaskData.from_stats_list(
            [
                HistoricalTestInformation(
                    test_name="a",
                    num_pass=1,
                    num_fail=0,
                    avg_duration_pass=1000,
                    max_duration_pass=1000,
                ),
                HistoricalTestInformation(
                    test_name="b",
                    num_pass=1,
                    num_fail=0,
                    avg_duration_pass=1,
                    max_duration_pass=1,
                ),
                HistoricalTestInformation(
                    test_name="c",
                    num_pass=1,
                    num_fail=0,
                    avg_duration_pass=1,
                    max_duration_pass=1,
                ),
                HistoricalTestInformation(
                    test_name="d",
                    num_pass=1,
                    num_fail=0,
                    avg_duration_pass=1,
                    max_duration_pass=1,
                ),
            ]
        )

        TestCase = namedtuple("TestCase", ["seed", "expected"])
        # The weighted shuffle is effective as long as 'a' is  prioritized to be earlier,
        # while all other equal runtime tests are completely random.
        testcases = [
            TestCase(0, ["c", "a", "b", "d"]),
            TestCase(1, ["a", "d", "b", "c"]),
            TestCase(2, ["d", "a", "c", "b"]),
            TestCase(3, ["a", "c", "b", "d"]),
            TestCase(4, ["a", "b", "c", "d"]),
            TestCase(5, ["a", "d", "b", "c"]),
            TestCase(6, ["c", "a", "b", "d"]),
            TestCase(7, ["a", "b", "d", "c"]),
            TestCase(8, ["a", "d", "c", "b"]),
            TestCase(9, ["a", "c", "b", "d"]),
        ]

        for testcase in testcases:
            random.seed(testcase.seed)
            actual = TestRunner.LongestFirstPartialShuffle(history).shuffle(tests)
            self.assertListEqual(
                actual, testcase.expected, f"Testcase with seed {testcase.seed} failed."
            )

    def test_slowest_first_partial_shuffle_empty(self):
        random.seed(0)
        history = HistoricTaskData.from_stats_list([])
        tests = ["a", "b", "c", "d"]
        expected = ["c", "a", "b", "d"]
        actual = TestRunner.LongestFirstPartialShuffle(history).shuffle(tests)
        self.assertListEqual(actual, expected)
