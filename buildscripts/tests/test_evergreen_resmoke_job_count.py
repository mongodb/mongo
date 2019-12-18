"""Unit tests for the evergreen_resomke_job_count script."""

import unittest

import psutil

from buildscripts import evergreen_resmoke_job_count as erjc

# pylint: disable=missing-docstring,no-self-use


class DetermineJobsTest(unittest.TestCase):
    cpu_count = psutil.cpu_count()
    mytask = "mytask"
    regex = "regexthatmatches"
    mytask_factor = 0.5
    regex_factor = 0.25
    task_factors = [{"task": mytask, "factor": mytask_factor},
                    {"task": "regex.*", "factor": regex_factor}]

    def test_determine_jobs_no_matching_task(self):
        jobs = erjc.determine_jobs("_no_match_", "_no_variant_", 0, 1)
        self.assertEqual(self.cpu_count, jobs)

    def test_determine_jobs_matching_variant(self):
        erjc.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": self.task_factors}
        jobs = erjc.determine_jobs(self.mytask, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = erjc.determine_jobs(self.regex, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_factor_matching_variant(self):
        erjc.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": self.task_factors}
        factor = erjc.determine_factor(self.mytask, "myvariant", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = erjc.determine_factor(self.regex, "myvariant", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_matching_machine(self):
        erjc.PLATFORM_MACHINE = "mymachine"
        erjc.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        jobs = erjc.determine_jobs(self.mytask, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = erjc.determine_jobs(self.regex, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_factor_matching_machine(self):
        erjc.PLATFORM_MACHINE = "mymachine"
        erjc.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        factor = erjc.determine_factor(self.mytask, "myvariant", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = erjc.determine_factor(self.regex, "myvariant", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_matching_platform(self):
        erjc.SYS_PLATFORM = "myplatform"
        erjc.PLATFORM_TASK_FACTOR_OVERRIDES = {"myplatform": self.task_factors}
        jobs = erjc.determine_jobs(self.mytask, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = erjc.determine_jobs(self.regex, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_factor_matching_platform(self):
        erjc.SYS_PLATFORM = "myplatform"
        erjc.PLATFORM_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        factor = erjc.determine_factor(self.mytask, "myvariant", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = erjc.determine_factor(self.regex, "myvariant", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_min_factor(self):
        erjc.PLATFORM_MACHINE = "mymachine"
        erjc.SYS_PLATFORM = "myplatform"
        mytask_factor_min = 0.5
        regex_factor_min = 0.25
        task_factors1 = [{"task": "mytask", "factor": mytask_factor_min + .5},
                         {"task": "regex.*", "factor": regex_factor_min + .5}]
        task_factors2 = [{"task": "mytask", "factor": mytask_factor_min + .25},
                         {"task": "regex.*", "factor": regex_factor_min + .25}]
        task_factors3 = [{"task": "mytask", "factor": mytask_factor_min},
                         {"task": "regex.*", "factor": regex_factor_min}]
        erjc.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": task_factors1}
        erjc.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": task_factors2}
        erjc.PLATFORM_TASK_FACTOR_OVERRIDES = {"myplatform": task_factors3}
        jobs = erjc.determine_jobs(self.mytask, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * mytask_factor_min)), jobs)
        jobs = erjc.determine_jobs(self.regex, "myvariant", 0, 1)
        self.assertEqual(int(round(self.cpu_count * regex_factor_min)), jobs)

    def test_determine_jobs_factor(self):
        factor = 0.4
        jobs = erjc.determine_jobs("_no_match_", "_no_variant_", 0, factor)
        self.assertEqual(int(round(self.cpu_count * factor)), jobs)

    def test_determine_jobs_jobs_max(self):
        jobs_max = 3
        jobs = erjc.determine_jobs("_no_match_", "_no_variant_", jobs_max, 1)
        self.assertEqual(min(jobs_max, jobs), jobs)
        jobs_max = 30
        jobs = erjc.determine_jobs("_no_match_", "_no_variant_", jobs_max, 1)
        self.assertEqual(min(jobs_max, jobs), jobs)
