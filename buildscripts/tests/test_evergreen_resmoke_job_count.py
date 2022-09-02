"""Unit tests for the evergreen_resomke_job_count script."""

import unittest

import psutil

from buildscripts import evergreen_resmoke_job_count as under_test


class DetermineJobsTest(unittest.TestCase):
    cpu_count = psutil.cpu_count()
    mytask = "mytask"
    regex = "regexthatmatches"
    mytask_factor = 0.5
    regex_factor = 0.25
    task_factors = [{"task": mytask, "factor": mytask_factor},
                    {"task": "regex.*", "factor": regex_factor}]

    def test_determine_jobs_no_matching_task(self):
        jobs = under_test.determine_jobs("_no_match_", "_no_variant_", "_no_distro_", 0, 1)
        self.assertEqual(self.cpu_count, jobs)

    def test_determine_jobs_matching_variant(self):
        under_test.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": self.task_factors}
        jobs = under_test.determine_jobs(self.mytask, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = under_test.determine_jobs(self.regex, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_jobs_matching_distro(self):
        old_multipliers = under_test.DISTRO_MULTIPLIERS
        try:
            under_test.DISTRO_MULTIPLIERS = {"mydistro": 3}
            under_test.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": self.task_factors}
            jobs = under_test.determine_jobs(self.mytask, "myvariant", "mydistro", 0, 1)
            self.assertEqual(int(round(self.cpu_count * self.mytask_factor * 3)), jobs)
            jobs = under_test.determine_jobs(self.regex, "myvariant", "mydistro", 0, 1)
            self.assertEqual(int(round(self.cpu_count * self.regex_factor * 3)), jobs)
        finally:
            under_test.DISTRO_MULTIPLIERS = old_multipliers

    def test_determine_factor_matching_variant(self):
        under_test.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": self.task_factors}
        factor = under_test.determine_factor(self.mytask, "myvariant", "mydistro", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = under_test.determine_factor(self.regex, "myvariant", "mydistro", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_matching_machine(self):
        under_test.PLATFORM_MACHINE = "mymachine"
        under_test.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        jobs = under_test.determine_jobs(self.mytask, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = under_test.determine_jobs(self.regex, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_factor_matching_machine(self):
        under_test.PLATFORM_MACHINE = "mymachine"
        under_test.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        factor = under_test.determine_factor(self.mytask, "myvariant", "mydistro", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = under_test.determine_factor(self.regex, "myvariant", "mydistro", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_matching_platform(self):
        under_test.SYS_PLATFORM = "myplatform"
        under_test.PLATFORM_TASK_FACTOR_OVERRIDES = {"myplatform": self.task_factors}
        jobs = under_test.determine_jobs(self.mytask, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.mytask_factor)), jobs)
        jobs = under_test.determine_jobs(self.regex, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * self.regex_factor)), jobs)

    def test_determine_factor_matching_platform(self):
        under_test.SYS_PLATFORM = "myplatform"
        under_test.PLATFORM_TASK_FACTOR_OVERRIDES = {"mymachine": self.task_factors}
        factor = under_test.determine_factor(self.mytask, "myvariant", "mydistro", 1)
        self.assertEqual(self.mytask_factor, factor)
        factor = under_test.determine_factor(self.regex, "myvariant", "mydistro", 1)
        self.assertEqual(self.regex_factor, factor)

    def test_determine_jobs_min_factor(self):
        under_test.PLATFORM_MACHINE = "mymachine"
        under_test.SYS_PLATFORM = "myplatform"
        mytask_factor_min = 0.5
        regex_factor_min = 0.25
        task_factors1 = [{"task": "mytask", "factor": mytask_factor_min + .5},
                         {"task": "regex.*", "factor": regex_factor_min + .5}]
        task_factors2 = [{"task": "mytask", "factor": mytask_factor_min + .25},
                         {"task": "regex.*", "factor": regex_factor_min + .25}]
        task_factors3 = [{"task": "mytask", "factor": mytask_factor_min},
                         {"task": "regex.*", "factor": regex_factor_min}]
        under_test.VARIANT_TASK_FACTOR_OVERRIDES = {"myvariant": task_factors1}
        under_test.MACHINE_TASK_FACTOR_OVERRIDES = {"mymachine": task_factors2}
        under_test.PLATFORM_TASK_FACTOR_OVERRIDES = {"myplatform": task_factors3}
        jobs = under_test.determine_jobs(self.mytask, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * mytask_factor_min)), jobs)
        jobs = under_test.determine_jobs(self.regex, "myvariant", "mydistro", 0, 1)
        self.assertEqual(int(round(self.cpu_count * regex_factor_min)), jobs)

    def test_determine_jobs_factor(self):
        factor = 0.4
        jobs = under_test.determine_jobs("_no_match_", "_no_variant_", "_no_distro_", 0, factor)
        self.assertEqual(int(round(self.cpu_count * factor)), jobs)

    def test_determine_jobs_jobs_max(self):
        jobs_max = 3
        jobs = under_test.determine_jobs("_no_match_", "_no_variant_", "_no_distro_", jobs_max, 1)
        self.assertEqual(min(jobs_max, jobs), jobs)
        jobs_max = 30
        jobs = under_test.determine_jobs("_no_match_", "_no_variant_", "_no_distro_", jobs_max, 1)
        self.assertEqual(min(jobs_max, jobs), jobs)

    def test_determine_jobs_with_global_specification(self):
        task = "matching_task"
        target_factor = 0.25
        jobs_default = 8
        under_test.CPU_COUNT = jobs_default
        under_test.GLOBAL_TASK_FACTOR_OVERRIDES = {
            r"matching.*": target_factor,
        }
        variant = "a_build_variant"
        distro = "a_distro"
        job_count_matching = under_test.determine_jobs(task, variant, distro, jobs_max=jobs_default)
        self.assertEqual(jobs_default * target_factor, job_count_matching)

    def test_determine_jobs_without_global_specification(self):
        task = "another_task"
        target_factor = 0.25
        jobs_default = 8
        under_test.CPU_COUNT = jobs_default
        under_test.GLOBAL_TASK_FACTOR_OVERRIDES = {
            r"matching.*": target_factor,
        }
        variant = "a_build_variant"
        distro = "a_distro"

        job_count_matching = under_test.determine_jobs(task, variant, distro, jobs_max=jobs_default)
        self.assertEqual(jobs_default, job_count_matching)
