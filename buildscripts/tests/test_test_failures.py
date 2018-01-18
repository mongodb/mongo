"""
Tests for buildscripts/test_failures.py.
"""

from __future__ import absolute_import

import datetime
import unittest

from buildscripts import test_failures


class TestReportEntry(unittest.TestCase):
    """
    Tests for the test_failures.ReportEntry class.
    """

    ENTRY = test_failures.ReportEntry(test="jstests/core/all.js",
                                      task="jsCore_WT",
                                      variant="linux-64",
                                      distro="rhel62",
                                      start_date=datetime.date(2017, 6, 3),
                                      end_date=datetime.date(2017, 6, 3),
                                      num_pass=0,
                                      num_fail=0)

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

        entry1 = self.ENTRY._replace(start_date=datetime.date(2017, 6, 1),
                                     end_date=datetime.date(2017, 6, 1),
                                     num_pass=1,
                                     num_fail=0)

        entry2 = self.ENTRY._replace(start_date=datetime.date(2017, 6, 2),
                                     end_date=datetime.date(2017, 6, 2),
                                     num_pass=0,
                                     num_fail=3)

        entry3 = self.ENTRY._replace(start_date=datetime.date(2017, 6, 3),
                                     end_date=datetime.date(2017, 6, 3),
                                     num_pass=0,
                                     num_fail=0)

        entry4 = self.ENTRY._replace(start_date=datetime.date(2017, 6, 4),
                                     end_date=datetime.date(2017, 6, 4),
                                     num_pass=2,
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

        entry1 = self.ENTRY._replace(test="jstests/core/all.js",
                                     task="jsCore_WT",
                                     variant="linux-64",
                                     distro="rhel62")

        entry2 = self.ENTRY._replace(test="jstests/core/all.js",
                                     task="jsCore_WT",
                                     variant="linux-64",
                                     distro="rhel55")

        entry3 = self.ENTRY._replace(test="jstests/core/all2.js",
                                     task="jsCore_WT",
                                     variant="linux-64-debug",
                                     distro="rhel62")

        entry4 = self.ENTRY._replace(test="jstests/core/all.js",
                                     task="jsCore",
                                     variant="linux-64-debug",
                                     distro="rhel62")

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

    ENTRY = test_failures.ReportEntry(test="jstests/core/all.js",
                                      task="jsCore_WT",
                                      variant="linux-64",
                                      distro="rhel62",
                                      start_date=datetime.date(2017, 6, 3),
                                      end_date=datetime.date(2017, 6, 3),
                                      num_pass=0,
                                      num_fail=0)

    ENTRIES = [
        ENTRY._replace(start_date=datetime.date(2017, 6, 3),
                       end_date=datetime.date(2017, 6, 3),
                       num_pass=1,
                       num_fail=0),
        ENTRY._replace(task="jsCore",
                       start_date=datetime.date(2017, 6, 5),
                       end_date=datetime.date(2017, 6, 5),
                       num_pass=0,
                       num_fail=1),
        ENTRY._replace(start_date=datetime.date(2017, 6, 10),
                       end_date=datetime.date(2017, 6, 10),
                       num_pass=1,
                       num_fail=0),
        # The following entry is intentionally not in timestamp order to verify that the
        # 'time_period' parameter becomes part of the sort in summarize_by().
        ENTRY._replace(start_date=datetime.date(2017, 6, 9),
                       end_date=datetime.date(2017, 6, 9),
                       num_pass=1,
                       num_fail=0),
        ENTRY._replace(distro="rhel55",
                       start_date=datetime.date(2017, 6, 10),
                       end_date=datetime.date(2017, 6, 10),
                       num_pass=0,
                       num_fail=1),
        ENTRY._replace(test="jstests/core/all2.js",
                       start_date=datetime.date(2017, 6, 10),
                       end_date=datetime.date(2017, 6, 10),
                       num_pass=1,
                       num_fail=0),
        ENTRY._replace(variant="linux-64-debug",
                       start_date=datetime.date(2017, 6, 17),
                       end_date=datetime.date(2017, 6, 17),
                       num_pass=0,
                       num_fail=1),
    ]

    def test_group_all_by_test_task_variant_distro(self):
        """
        Tests that summarize_by() correctly accumulates all unique combinations of
        (test, task, variant, distro).
        """

        report = test_failures.Report(self.ENTRIES)
        summed_entries = report.summarize_by(test_failures.Report.TEST_TASK_VARIANT_DISTRO)
        self.assertEqual(5, len(summed_entries))
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task="jsCore",
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 5),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            distro="rhel55",
            start_date=datetime.date(2017, 6, 10),
            end_date=datetime.date(2017, 6, 10),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 10),
            num_pass=3,
            num_fail=0,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 17),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[4], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task="jsCore",
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 5),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 10),
            num_pass=3,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 17),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task="jsCore",
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 5),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            variant=test_failures.Wildcard("variants"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 17),
            num_pass=3,
            num_fail=2,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            variant=test_failures.Wildcard("variants"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 17),
            num_pass=3,
            num_fail=3,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task="jsCore",
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 5),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            test=test_failures.Wildcard("tests"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 10),
            num_pass=4,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 3),
            num_pass=1,
            num_fail=0,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 4),
            end_date=datetime.date(2017, 6, 10),
            num_pass=2,
            num_fail=2,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 11),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 4),
            num_pass=1,
            num_fail=0,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 11),
            num_pass=2,
            num_fail=2,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 12),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 6),
            num_pass=1,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 7),
            end_date=datetime.date(2017, 6, 13),
            num_pass=2,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 14),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 3),
            num_pass=1,
            num_fail=0,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            task="jsCore",
            start_date=datetime.date(2017, 6, 5),
            end_date=datetime.date(2017, 6, 5),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            start_date=datetime.date(2017, 6, 9),
            end_date=datetime.date(2017, 6, 9),
            num_pass=1,
            num_fail=0,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 10),
            end_date=datetime.date(2017, 6, 10),
            num_pass=1,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[4], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 17),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[5], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 6),
            num_pass=1,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 7),
            end_date=datetime.date(2017, 6, 10),
            num_pass=2,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 15),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[3], self.ENTRY._replace(
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
        self.assertEqual(summed_entries[0], self.ENTRY._replace(
            task=test_failures.Wildcard("tasks"),
            distro=test_failures.Wildcard("distros"),
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 11),
            num_pass=3,
            num_fail=2,
        ))
        self.assertEqual(summed_entries[1], self.ENTRY._replace(
            variant="linux-64-debug",
            start_date=datetime.date(2017, 6, 12),
            end_date=datetime.date(2017, 6, 17),
            num_pass=0,
            num_fail=1,
        ))
        self.assertEqual(summed_entries[2], self.ENTRY._replace(
            test="jstests/core/all2.js",
            start_date=datetime.date(2017, 6, 3),
            end_date=datetime.date(2017, 6, 11),
            num_pass=1,
            num_fail=0,
        ))
