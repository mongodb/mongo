"""
Tests for buildscripts/test_failures.py.
"""

from __future__ import absolute_import

import datetime
import unittest

from buildscripts import lifecycle_test_failures as test_failures
from buildscripts.client import evergreen as evergreen

# pylint: disable=attribute-defined-outside-init,invalid-name,missing-docstring,protected-access


class TestReportEntry(unittest.TestCase):
    """
    Tests for the test_failures.ReportEntry class.
    """

    ENTRY = test_failures.ReportEntry(test="jstests/core/all.js", task="jsCore_WT",
                                      variant="linux-64", distro="rhel62", start_date=datetime.date(
                                          2017, 6, 3), end_date=datetime.date(2017, 6, 3),
                                      num_pass=0, num_fail=0)

    def test_fail_rate(self):
        """
        Tests for the test_failures.ReportEntry.fail_rate property.
        """

        entry = self.ENTRY._replace(num_pass=0, num_fail=1)
        self.assertEqual(1, entry.fail_rate)

        entry = self.ENTRY._replace(num_pass=9, num_fail=1)
        self.assertAlmostEqual(0.1, entry.fail_rate)

        # Verify that we don't attempt to divide by zero.
        entry = self.ENTRY._replace(num_pass=0, num_fail=0)
        self.assertEqual(0, entry.fail_rate)

    def test_week_start_date_with_sunday(self):
        """
        Tests for test_failures.ReportEntry.week_start_date() with the beginning of the week
        specified as different forms of the string "Sunday".
        """

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 3))
        self.assertEqual(datetime.date(2017, 5, 28), entry.week_start_date("sunday"))
        self.assertEqual(datetime.date(2017, 5, 28), entry.week_start_date("Sunday"))
        self.assertEqual(datetime.date(2017, 5, 28), entry.week_start_date("SUNDAY"))

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 4))
        self.assertEqual(datetime.date(2017, 6, 4), entry.week_start_date("sunday"))

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 5))
        self.assertEqual(datetime.date(2017, 6, 4), entry.week_start_date("sunday"))

    def test_week_start_date_with_monday(self):
        """
        Tests for test_failures.ReportEntry.week_start_date() with the beginning of the week
        specified as different forms of the string "Monday".
        """

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 3))
        self.assertEqual(datetime.date(2017, 5, 29), entry.week_start_date("monday"))
        self.assertEqual(datetime.date(2017, 5, 29), entry.week_start_date("Monday"))
        self.assertEqual(datetime.date(2017, 5, 29), entry.week_start_date("MONDAY"))

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 4))
        self.assertEqual(datetime.date(2017, 5, 29), entry.week_start_date("monday"))

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 5))
        self.assertEqual(datetime.date(2017, 6, 5), entry.week_start_date("monday"))

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 6))
        self.assertEqual(datetime.date(2017, 6, 5), entry.week_start_date("monday"))

    def test_week_start_date_with_date(self):
        """
        Tests for test_failures.ReportEntry.week_start_date() with the beginning of the week
        specified as a datetime.date() value.
        """

        entry = self.ENTRY._replace(start_date=datetime.date(2017, 6, 3))

        date = datetime.date(2017, 5, 21)
        self.assertEqual(6, date.weekday(), "2017 May 21 is a Sunday")
        self.assertEqual(datetime.date(2017, 5, 28), entry.week_start_date(date))

        date = datetime.date(2017, 5, 22)
        self.assertEqual(0, date.weekday(), "2017 May 22 is a Monday")
        self.assertEqual(datetime.date(2017, 5, 29), entry.week_start_date(date))

        date = datetime.date(2017, 6, 6)
        self.assertEqual(1, date.weekday(), "2017 Jun 06 is a Tuesday")
        self.assertEqual(datetime.date(2017, 5, 30), entry.week_start_date(date))

        date = datetime.date(2017, 6, 9)
        self.assertEqual(4, date.weekday(), "2017 Jun 09 is a Friday")
        self.assertEqual(datetime.date(2017, 6, 2), entry.week_start_date(date))

        date = datetime.date(2017, 6, 3)
        self.assertEqual(5, date.weekday(), "2017 Jun 03 is a Saturday")
        self.assertEqual(datetime.date(2017, 6, 3), entry.week_start_date(date))

    def test_sum_combines_test_results(self):
        """
        Tests for test_failures.ReportEntry.sum() that verify the start_date, end_date, num_pass,
        and num_fail attributes are accumulated correctly.
        """

        entry1 = self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 1), end_date=datetime.date(2017, 6, 1), num_pass=1,
            num_fail=0)

        entry2 = self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 2), end_date=datetime.date(2017, 6, 2), num_pass=0,
            num_fail=3)

        entry3 = self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 3), end_date=datetime.date(2017, 6, 3), num_pass=0,
            num_fail=0)

        entry4 = self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 4), end_date=datetime.date(2017, 6, 4), num_pass=2,
            num_fail=2)

        entry_1234 = test_failures.ReportEntry.sum([entry1, entry2, entry3, entry4])
        entry_1432 = test_failures.ReportEntry.sum([entry1, entry4, entry3, entry2])
        entry_124 = test_failures.ReportEntry.sum([entry1, entry2, entry4])
        entry_13 = test_failures.ReportEntry.sum([entry1, entry3])
        entry_42 = test_failures.ReportEntry.sum([entry4, entry2])

        self.assertEqual(datetime.date(2017, 6, 1), entry_1234.start_date)
        self.assertEqual(datetime.date(2017, 6, 4), entry_1234.end_date)
        self.assertEqual(3, entry_1234.num_pass)
        self.assertEqual(5, entry_1234.num_fail)

        self.assertEqual(entry_1234, entry_1432, "order of arguments shouldn't matter")
        self.assertEqual(entry_1234, entry_124, "entry3 didn't have any test executions")

        self.assertEqual(datetime.date(2017, 6, 1), entry_13.start_date)
        self.assertEqual(datetime.date(2017, 6, 3), entry_13.end_date)
        self.assertEqual(1, entry_13.num_pass)
        self.assertEqual(0, entry_13.num_fail)

        self.assertEqual(datetime.date(2017, 6, 2), entry_42.start_date)
        self.assertEqual(datetime.date(2017, 6, 4), entry_42.end_date)
        self.assertEqual(2, entry_42.num_pass)
        self.assertEqual(5, entry_42.num_fail)

    def test_sum_combines_test_info(self):
        """
        Tests for test_failures.ReportEntry.sum() that verify the test, task, variant, and distro
        attributes are accumulated correctly.
        """

        entry1 = self.ENTRY._replace(test="jstests/core/all.js", task="jsCore_WT",
                                     variant="linux-64", distro="rhel62")

        entry2 = self.ENTRY._replace(test="jstests/core/all.js", task="jsCore_WT",
                                     variant="linux-64", distro="rhel55")

        entry3 = self.ENTRY._replace(test="jstests/core/all2.js", task="jsCore_WT",
                                     variant="linux-64-debug", distro="rhel62")

        entry4 = self.ENTRY._replace(test="jstests/core/all.js", task="jsCore",
                                     variant="linux-64-debug", distro="rhel62")

        entry_12 = test_failures.ReportEntry.sum([entry1, entry2])
        self.assertEqual("jstests/core/all.js", entry_12.test)
        self.assertEqual("jsCore_WT", entry_12.task)
        self.assertEqual("linux-64", entry_12.variant)
        self.assertIsInstance(entry_12.distro, test_failures.Wildcard)

        entry_123 = test_failures.ReportEntry.sum([entry1, entry2, entry3])
        self.assertIsInstance(entry_123.test, test_failures.Wildcard)
        self.assertEqual("jsCore_WT", entry_123.task)
        self.assertIsInstance(entry_123.variant, test_failures.Wildcard)
        self.assertIsInstance(entry_123.distro, test_failures.Wildcard)

        entry_1234 = test_failures.ReportEntry.sum([entry1, entry2, entry3, entry4])
        self.assertIsInstance(entry_1234.test, test_failures.Wildcard)
        self.assertIsInstance(entry_1234.task, test_failures.Wildcard)
        self.assertIsInstance(entry_1234.variant, test_failures.Wildcard)
        self.assertIsInstance(entry_1234.distro, test_failures.Wildcard)

        entry_34 = test_failures.ReportEntry.sum([entry3, entry4])
        self.assertIsInstance(entry_34.test, test_failures.Wildcard)
        self.assertIsInstance(entry_34.task, test_failures.Wildcard)
        self.assertEqual("linux-64-debug", entry_34.variant)
        self.assertEqual("rhel62", entry_34.distro)


class TestReportSummarization(unittest.TestCase):
    """
    Tests for test_failures.Report.summarize_by().
    """

    ENTRY = test_failures.ReportEntry(test="jstests/core/all.js", task="jsCore_WT",
                                      variant="linux-64", distro="rhel62", start_date=datetime.date(
                                          2017, 6, 3), end_date=datetime.date(2017, 6, 3),
                                      num_pass=0, num_fail=0)

    ENTRIES = [
        ENTRY._replace(
            start_date=datetime.date(2017, 6, 3), end_date=datetime.date(2017, 6, 3), num_pass=1,
            num_fail=0),
        ENTRY._replace(task="jsCore", start_date=datetime.date(2017, 6, 5), end_date=datetime.date(
            2017, 6, 5), num_pass=0, num_fail=1),
        ENTRY._replace(
            start_date=datetime.date(2017, 6, 10), end_date=datetime.date(2017, 6, 10), num_pass=1,
            num_fail=0),
        # The following entry is intentionally not in timestamp order to verify that the
        # 'time_period' parameter becomes part of the sort in summarize_by().
        ENTRY._replace(
            start_date=datetime.date(2017, 6, 9), end_date=datetime.date(2017, 6, 9), num_pass=1,
            num_fail=0),
        ENTRY._replace(distro="rhel55", start_date=datetime.date(2017, 6, 10),
                       end_date=datetime.date(2017, 6, 10), num_pass=0, num_fail=1),
        ENTRY._replace(test="jstests/core/all2.js", start_date=datetime.date(2017, 6, 10),
                       end_date=datetime.date(2017, 6, 10), num_pass=1, num_fail=0),
        ENTRY._replace(variant="linux-64-debug", start_date=datetime.date(2017, 6, 17),
                       end_date=datetime.date(2017, 6, 17), num_pass=0, num_fail=1),
    ]

    def test_group_all_by_test_task_variant_distro(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of
        (test, task, variant, distro).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST_TASK_VARIANT_DISTRO)
        self.assertEqual(5, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task="jsCore",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 5),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             distro="rhel55",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=3,
                             num_fail=0,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 17),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[4],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_all_by_test_task_variant(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of
        (test, task, variant).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST_TASK_VARIANT)
        self.assertEqual(4, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task="jsCore",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 5),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=3,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 17),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_all_by_test_task(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of (test, task).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST_TASK)
        self.assertEqual(3, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task="jsCore",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 5),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             variant=test_failures.Wildcard("variants"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=3,
                             num_fail=2,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_all_by_test(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of (test,).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST)
        self.assertEqual(2, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             variant=test_failures.Wildcard("variants"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=3,
                             num_fail=3,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_all_by_variant_task(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of (variant, task).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(["variant", "task"])
        self.assertEqual(3, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task="jsCore",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 5),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             test=test_failures.Wildcard("tests"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=4,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 17),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))

    def test_group_weekly_by_test_starting_on_sunday(self):
        """
        Tests that summarize_by() correctly accumulates by week when the beginning of the week is
        specified as the string "sunday".
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=test_failures.Report.WEEKLY,
                                             start_day_of_week=test_failures.Report.SUNDAY)

        self.assertEqual(4, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 3),
                             num_pass=1,
                             num_fail=0,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 4),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=2,
                             num_fail=2,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 11),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 4),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_weekly_by_test_starting_on_monday(self):
        """
        Tests that summarize_by() correctly accumulates by week when the beginning of the week is
        specified as the string "monday".
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=test_failures.Report.WEEKLY,
                                             start_day_of_week=test_failures.Report.MONDAY)

        self.assertEqual(4, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 4),
                             num_pass=1,
                             num_fail=0,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 11),
                             num_pass=2,
                             num_fail=2,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 12),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 11),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_weekly_by_test_starting_on_date(self):
        """
        Tests that summarize_by() correctly accumulates by week when the beginning of the week is
        specified as a datetime.date() value.
        """

        date = datetime.date(2017, 6, 7)
        self.assertEqual(2, date.weekday(), "2017 Jun 07 is a Wednesday")

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=test_failures.Report.WEEKLY,
                                             start_day_of_week=date)

        self.assertEqual(4, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 6),
                             num_pass=1,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 7),
                             end_date=datetime.date(2017, 6, 13),
                             num_pass=2,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 14),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 7),
                             end_date=datetime.date(2017, 6, 13),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_daily_by_test(self):
        """
        Tests that summarize_by() correctly accumulates by day.
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=test_failures.Report.DAILY)

        self.assertEqual(6, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 3),
                             num_pass=1,
                             num_fail=0,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             task="jsCore",
                             start_date=datetime.date(2017, 6, 5),
                             end_date=datetime.date(2017, 6, 5),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             start_date=datetime.date(2017, 6, 9),
                             end_date=datetime.date(2017, 6, 9),
                             num_pass=1,
                             num_fail=0,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[4],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 17),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[5],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 10),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_4days_by_test(self):
        """
        Tests that summarize_by() correctly accumulates by multiple days.
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=datetime.timedelta(days=4))

        self.assertEqual(4, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 6),
                             num_pass=1,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 7),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=2,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 15),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[3],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 7),
                             end_date=datetime.date(2017, 6, 10),
                             num_pass=1,
                             num_fail=0,
                         ))

    def test_group_9days_by_test(self):
        """
        Tests that summarize_by() correctly accumulates by multiple days, including time periods
        greater than 1 week.
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST,
                                             time_period=datetime.timedelta(days=9))

        self.assertEqual(3, len(summed_entries))
        self.assertEqual(summed_entries[0],
                         self.ENTRY._replace(
                             task=test_failures.Wildcard("tasks"),
                             distro=test_failures.Wildcard("distros"),
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 11),
                             num_pass=3,
                             num_fail=2,
                         ))
        self.assertEqual(summed_entries[1],
                         self.ENTRY._replace(
                             variant="linux-64-debug",
                             start_date=datetime.date(2017, 6, 12),
                             end_date=datetime.date(2017, 6, 17),
                             num_pass=0,
                             num_fail=1,
                         ))
        self.assertEqual(summed_entries[2],
                         self.ENTRY._replace(
                             test="jstests/core/all2.js",
                             start_date=datetime.date(2017, 6, 3),
                             end_date=datetime.date(2017, 6, 11),
                             num_pass=1,
                             num_fail=0,
                         ))


class mockEvergreenApiV2(object):
    """Mock class for EvergreenApiV2."""
    pass


def _get_evergreen_apiv2(  # pylint: disable=unused-argument
        api_server=None, api_headers=None, num_retries=0):
    """Mock function for evergreen.get_evergreen_apiv2."""
    return mockEvergreenApiV2()


class TestHistoryTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        evergreen.get_evergreen_apiv2 = _get_evergreen_apiv2
        cls.test_history = test_failures.TestHistory()

    @staticmethod
    def dt_to_str(dt):
        return dt.strftime("%Y-%m-%d")

    def _test_stats(  # pylint: disable=unused-argument,too-many-arguments
            self, project, after_date, before_date, group_num_days=None, requester=None, sort=None,
            limit=None, tests=None, tasks=None, variants=None, distros=None):
        return self.my_api_results


class TestHistoryTests(TestHistoryTestCase):
    """
    Tests for test_failures.TestHistory.
    """

    def test_by_date(self):
        """
        Tests get_history_by_date.
        """
        my_date = "2018-01-01"
        my_tests = ["jstests/core/and3.js", "jstests/core/or3.js"]
        my_passes = [11, 6]
        my_fails = [2, 3]
        self.my_api_results = []
        for i, _ in enumerate(my_tests):
            self.my_api_results.append({
                "test_file": my_tests[i], "date": my_date, "num_pass": my_passes[i],
                "num_fail": my_fails[i], "avg_duration_pass": 1.1
            })
        mockEvergreenApiV2.test_stats = self._test_stats
        history_data = self.test_history.get_history_by_date(my_date, my_date)
        self.assertEqual(len(self.my_api_results), len(history_data))
        for i in range(len(self.my_api_results)):
            self.assertEqual(my_tests[i], getattr(history_data[i], "test"))
            self.assertEqual(my_passes[i], getattr(history_data[i], "num_pass"))
            self.assertEqual(my_fails[i], getattr(history_data[i], "num_fail"))
            self.assertEqual(my_date, self.dt_to_str(getattr(history_data[i], "start_date")))
            self.assertEqual(my_date, self.dt_to_str(getattr(history_data[i], "end_date")))
            self.assertEqual("<multiple tasks>", str(getattr(history_data[i], "task")))
            self.assertEqual("<multiple variants>", str(getattr(history_data[i], "variant")))
            self.assertEqual("<multiple distros>", str(getattr(history_data[i], "distro")))

    def test_by_date_windows_file(self):
        """
        Tests get_history_by_date with a Windows named test file.
        """
        my_date = "2018-01-01"
        my_test = "jstests\\core\\and3.js"
        my_pass = 11
        my_fail = 2
        self.my_api_results = [{
            "test_file": my_test, "date": my_date, "num_pass": my_pass, "num_fail": my_fail,
            "avg_duration_pass": 1.1
        }]
        mockEvergreenApiV2.test_stats = self._test_stats
        history_data = self.test_history.get_history_by_date(my_date, my_date)
        self.assertEqual(1, len(history_data))
        self.assertEqual("jstests/core/and3.js", getattr(history_data[0], "test"))

    def test_by_date_tasks(self):
        """
        Tests get_history_by_date with tasks specified.
        """
        my_date = "2018-01-01"
        my_test = "jstests/core/and3.js"
        my_pass = 11
        my_fail = 2
        my_task = "jsCore"
        self.my_api_results = [{
            "test_file": my_test, "task_name": my_task, "date": my_date, "num_pass": my_pass,
            "num_fail": my_fail, "avg_duration_pass": 1.1
        }]
        mockEvergreenApiV2.test_stats = self._test_stats
        history_data = self.test_history.get_history_by_date(my_date, my_date)
        self.assertEqual(1, len(history_data))
        self.assertEqual(my_test, getattr(history_data[0], "test"))
        self.assertEqual(my_pass, getattr(history_data[0], "num_pass"))
        self.assertEqual(my_fail, getattr(history_data[0], "num_fail"))
        self.assertEqual(my_date, self.dt_to_str(getattr(history_data[0], "start_date")))
        self.assertEqual(my_date, self.dt_to_str(getattr(history_data[0], "end_date")))
        self.assertEqual(my_task, getattr(history_data[0], "task"))
        self.assertEqual("<multiple variants>", str(getattr(history_data[0], "variant")))
        self.assertEqual("<multiple distros>", str(getattr(history_data[0], "distro")))

    def test_by_date_tasks_variants(self):
        """
        Tests get_history_by_date with tasks & variants specified.
        """
        my_date = "2018-01-01"
        my_test = "jstests/core/and3.js"
        my_variant = "linux-64"
        my_pass = 11
        my_fail = 2
        my_task = "jsCore"
        self.my_api_results = [{
            "test_file": my_test, "task_name": my_task, "variant": my_variant, "date": my_date,
            "num_pass": my_pass, "num_fail": my_fail, "avg_duration_pass": 1.1
        }]
        mockEvergreenApiV2.test_stats = self._test_stats
        history_data = self.test_history.get_history_by_date(my_date, my_date)
        self.assertEqual(1, len(history_data))
        self.assertEqual(my_test, getattr(history_data[0], "test"))
        self.assertEqual(my_pass, getattr(history_data[0], "num_pass"))
        self.assertEqual(my_fail, getattr(history_data[0], "num_fail"))
        self.assertEqual(my_date, self.dt_to_str(getattr(history_data[0], "start_date")))
        self.assertEqual(my_date, self.dt_to_str(getattr(history_data[0], "end_date")))
        self.assertEqual(my_task, getattr(history_data[0], "task"))
        self.assertEqual(my_variant, getattr(history_data[0], "variant"))
        self.assertEqual("<multiple distros>", str(getattr(history_data[0], "distro")))

    def test_by_date_tasks_variants_distros(self):
        """
        Tests get_history_by_date with tasks, variants & distros specified.
        """
        my_date1 = "2018-01-01"
        my_date2 = "2019-01-01"
        my_test = "jstests/core/and3.js"
        my_variant = "linux-64"
        my_distro = "rhel62-large"
        my_pass = 11
        my_fail = 2
        my_task = "jsCore"
        self.my_api_results = [{
            "test_file": my_test, "task_name": my_task, "variant": my_variant, "distro": my_distro,
            "date": my_date1, "num_pass": my_pass, "num_fail": my_fail, "avg_duration_pass": 1.1
        }]
        mockEvergreenApiV2.test_stats = self._test_stats
        history_data = self.test_history.get_history_by_date(my_date1, my_date2)
        self.assertEqual(1, len(history_data))
        self.assertEqual(my_test, getattr(history_data[0], "test"))
        self.assertEqual(my_pass, getattr(history_data[0], "num_pass"))
        self.assertEqual(my_fail, getattr(history_data[0], "num_fail"))
        self.assertEqual(my_date1, self.dt_to_str(getattr(history_data[0], "start_date")))
        self.assertEqual(my_date1, self.dt_to_str(getattr(history_data[0], "end_date")))
        self.assertEqual(my_task, getattr(history_data[0], "task"))
        self.assertEqual(my_variant, getattr(history_data[0], "variant"))
        self.assertEqual(my_distro, getattr(history_data[0], "distro"))
