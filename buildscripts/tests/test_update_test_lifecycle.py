"""
Tests for buildscripts/update_test_lifecycle.py.
"""

from __future__ import absolute_import

import collections
import copy
import datetime
import unittest

from buildscripts import test_failures
from buildscripts import update_test_lifecycle
from buildscripts.ciconfig import tags as ci_tags


class TestValidateConfig(unittest.TestCase):
    """
    Tests for the validate_config() function.
    """

    CONFIG = update_test_lifecycle.Config(
        test_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        task_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        variant_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        distro_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        reliable_min_runs=2,
        reliable_time_period=datetime.timedelta(days=1),
        unreliable_min_runs=2,
        unreliable_time_period=datetime.timedelta(days=1))

    def test_acceptable_test_fail_rate(self):
        """
        Tests the validation of the 'test_fail_rates.acceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_unacceptable_test_fail_rate(self):
        """
        Tests the validation of the 'test_fail_rates.unacceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_test_fail_rates(self):
        """
        Tests the validation of the 'test_fail_rates' attribute.
        """

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9,
                                                                     unacceptable=0.1))
            update_test_lifecycle.validate_config(config)

    def test_acceptable_task_fail_rate(self):
        """
        Tests the validation of the 'test_fail_rates.acceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_unacceptable_task_fail_rate(self):
        """
        Tests the validation of the 'task_fail_rates.unacceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_task_fail_rates(self):
        """
        Tests the validation of the 'task_fail_rates' attribute.
        """

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9,
                                                                     unacceptable=0.1))
            update_test_lifecycle.validate_config(config)

    def test_acceptable_variant_fail_rate(self):
        """
        Tests the validation of the 'variant_fail_rates.acceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(
                    acceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_unacceptable_variant_fail_rate(self):
        """
        Tests the validation of the 'variant_fail_rates.unacceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(
                    unacceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_variant_fail_rates(self):
        """
        Tests the validation of the 'variant_fail_rates' attribute.
        """

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9,
                                                                           unacceptable=0.1))
            update_test_lifecycle.validate_config(config)

    def test_acceptable_distro_fail_rate(self):
        """
        Tests the validation of the 'distro_fail_rates.acceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_unacceptable_distro_fail_rate(self):
        """
        Tests the validation of the 'distro_fail_rates.unacceptable' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(
                    unacceptable="not a number"))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=2))
            update_test_lifecycle.validate_config(config)

    def test_distro_fail_rates(self):
        """
        Tests the validation of the 'distro_fail_rates' attribute.
        """

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9,
                                                                         unacceptable=0.1))
            update_test_lifecycle.validate_config(config)

    def test_reliable_min_runs(self):
        """
        Tests the validation of the 'reliable_min_runs' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(reliable_min_runs="not a number")
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_min_runs=-1)
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_min_runs=0)
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_min_runs=1.5)
            update_test_lifecycle.validate_config(config)

    def test_reliable_time_period(self):
        """
        Tests the validation of the 'reliable_time_period' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(reliable_time_period="not a datetime.timedelta")
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_time_period=datetime.timedelta(days=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_time_period=datetime.timedelta(days=0))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(reliable_time_period=datetime.timedelta(days=1, hours=1))
            update_test_lifecycle.validate_config(config)

    def test_unreliable_min_runs(self):
        """
        Tests the validation of the 'unreliable_min_runs' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(unreliable_min_runs="not a number")
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(unreliable_min_runs=-1)
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(unreliable_min_runs=0)
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(unreliable_min_runs=1.5)
            update_test_lifecycle.validate_config(config)

    def test_unreliable_time_period(self):
        """
        Tests the validation of the 'unreliable_time_period' attribute.
        """

        with self.assertRaises(TypeError):
            config = self.CONFIG._replace(unreliable_time_period="not a datetime.timedelta")
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(unreliable_time_period=datetime.timedelta(days=-1))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(unreliable_time_period=datetime.timedelta(days=0))
            update_test_lifecycle.validate_config(config)

        with self.assertRaises(ValueError):
            config = self.CONFIG._replace(
                unreliable_time_period=datetime.timedelta(days=1, hours=1))
            update_test_lifecycle.validate_config(config)


class TestUpdateTags(unittest.TestCase):
    """
    Tests for the update_tags() function.
    """

    CONFIG = update_test_lifecycle.Config(
        test_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        task_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        variant_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        distro_fail_rates=update_test_lifecycle.Rates(acceptable=0, unacceptable=1),
        reliable_min_runs=2,
        reliable_time_period=datetime.timedelta(days=1),
        unreliable_min_runs=2,
        unreliable_time_period=datetime.timedelta(days=1))

    ENTRY = test_failures.ReportEntry(test="jstests/core/all.js",
                                      task="jsCore_WT",
                                      variant="linux-64",
                                      distro="rhel62",
                                      start_date=datetime.date(2017, 6, 3),
                                      end_date=datetime.date(2017, 6, 3),
                                      num_pass=0,
                                      num_fail=0)

    def assert_has_only_js_tests(self, lifecycle):
        """
        Raises an AssertionError exception if 'lifecycle' is not of the following form:

            selector:
              js_test:
                ...
        """

        self.assertIn("selector", lifecycle.raw)
        self.assertEqual(1, len(lifecycle.raw), msg=str(lifecycle.raw))
        self.assertIn("js_test", lifecycle.raw["selector"])
        self.assertEqual(1, len(lifecycle.raw["selector"]), msg=str(lifecycle.raw))

        return lifecycle.raw["selector"]["js_test"]

    def transition_from_reliable_to_unreliable(self, config, expected_tags):
        """
        Tests that update_tags() tags a formerly reliable combination as being unreliable.
        """

        initial_tags = collections.OrderedDict()
        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(collections.OrderedDict(), self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, expected_tags)

    def test_transition_test_from_reliable_to_unreliable(self):
        """
        Tests that update_tags() tags a formerly reliable (test,) combination as being unreliable.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1))

        self.transition_from_reliable_to_unreliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable"]),
        ]))

    def test_transition_task_from_reliable_to_unreliable(self):
        """
        Tests that update_tags() tags a formerly reliable (test, task) combination as being
        unreliable.
        """

        config = self.CONFIG._replace(
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1))

        self.transition_from_reliable_to_unreliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT"]),
        ]))

    def test_transition_variant_from_reliable_to_unreliable(self):
        """
        Tests that update_tags() tags a formerly reliable (test, task, variant) combination as being
        unreliable.
        """

        config = self.CONFIG._replace(
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1))

        self.transition_from_reliable_to_unreliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT|linux-64"]),
        ]))

    def test_transition_distro_from_reliable_to_unreliable(self):
        """
        Tests that update_tags() tags a formerly reliable (test, task, variant, distro) combination
        as being unreliable.
        """

        config = self.CONFIG._replace(
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1))

        self.transition_from_reliable_to_unreliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT|linux-64|rhel62"]),
        ]))

    def test_transition_from_reliable_to_unreliable(self):
        """
        Tests that update_tags() tags multiple formerly reliable combination as being unreliable.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1))

        self.transition_from_reliable_to_unreliable(config, collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ]))

    def transition_from_unreliable_to_reliable(self, config, initial_tags):
        """
        Tests that update_tags() untags a formerly unreliable combination after it has become
        reliable again.
        """

        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0, task="jsCore"),
            self.ENTRY._replace(num_pass=1, num_fail=0, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=1, num_fail=0, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, collections.OrderedDict())

    def test_non_running_in_reliable_period_is_reliable(self):
        """
        Tests that tests that have a failure rate above the unacceptable rate during the unreliable
        period but haven't run during the reliable period are marked as reliable.
        """
        # Unreliable period is 2 days: 2017-06-03 to 2017-06-04.
        # Reliable period is 1 day: 2016-06-04.
        reliable_period_date = datetime.date(2017, 6, 4)
        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1),
            unreliable_time_period=datetime.timedelta(days=2))

        tests = ["jstests/core/all.js"]
        initial_tags = collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ])

        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        # The test did not run on the reliable period on linux-64.
        report = test_failures.Report([
            # Failing.
            self.ENTRY._replace(num_pass=0,
                                num_fail=2),
            # Passing on a different variant.
            self.ENTRY._replace(start_date=reliable_period_date,
                                end_date=reliable_period_date,
                                num_pass=3,
                                num_fail=0,
                                variant="linux-alt",
                                distro="debian7"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        # The tags for variant and distro have been removed.
        self.assertEqual(updated_tags, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable", "unreliable|jsCore_WT"])]))

    def test_non_running_at_all_is_reliable(self):
        """
        Tests that tests that are tagged as unreliable but no longer running (either during the
        reliable or the unreliable period) have their tags removed.
        """
        config = self.CONFIG

        tests = ["jstests/core/all.js", "jstests/core/all2.js"]
        initial_tags = collections.OrderedDict([
            ("jstests/core/all2.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ])

        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        # all2.js did not run at all
        report = test_failures.Report([self.ENTRY])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        # The tags for variant and distro have been removed.
        self.assertEqual(updated_tags, collections.OrderedDict([]))

    def test_transition_test_from_unreliable_to_reliable(self):
        """
        Tests that update_tags() untags a formerly unreliable (test,) combination after it has
        become reliable again.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9))

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable"]),
        ]))

    def test_transition_task_from_unreliable_to_reliable(self):
        """
        Tests that update_tags() untags a formerly unreliable (test, task) combination after it has
        become reliable again.
        """

        config = self.CONFIG._replace(
            task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9))

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT"]),
        ]))

    def test_transition_variant_from_unreliable_to_reliable(self):
        """
        Tests that update_tags() untags a formerly unreliable (test, task, variant) combination
        after it has become reliable again.
        """

        config = self.CONFIG._replace(
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9))

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT|linux-64"]),
        ]))

    def test_transition_distro_from_unreliable_to_reliable(self):
        """
        Tests that update_tags() untags a formerly unreliable (test, task, variant, distro)
        combination after it has become reliable again.
        """

        config = self.CONFIG._replace(
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9))

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", ["unreliable|jsCore_WT|linux-64|rhel62"]),
        ]))

    def test_transition_from_unreliable_to_reliable(self):
        """
        Tests that update_tags() untags multiple formerly unreliable combination after it has become
        reliable again.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9))

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ]))

    def test_remain_reliable(self):
        """
        Tests that update_tags() preserves the absence of tags for reliable combinations.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9))

        initial_tags = collections.OrderedDict()
        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0, task="jsCore"),
            self.ENTRY._replace(num_pass=1, num_fail=0, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=1, num_fail=0, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, initial_tags)

    def test_remain_unreliable(self):
        """
        Tests that update_tags() preserves the tags for unreliable combinations.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1))

        initial_tags = collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ])

        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, initial_tags)

    def test_obeys_reliable_min_runs(self):
        """
        Tests that update_tags() considers a test reliable if it has fewer than 'reliable_min_runs'.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9),
            reliable_min_runs=100)

        self.transition_from_unreliable_to_reliable(config, collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ]))

    def test_obeys_reliable_time_period(self):
        """
        Tests that update_tags() ignores passes from before 'reliable_time_period'.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(acceptable=0.9),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(acceptable=0.9),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(acceptable=0.9),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(acceptable=0.9))

        initial_tags = collections.OrderedDict()
        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(start_date=(self.ENTRY.start_date - datetime.timedelta(days=1)),
                                end_date=(self.ENTRY.end_date - datetime.timedelta(days=1)),
                                num_pass=1,
                                num_fail=0),
            self.ENTRY._replace(start_date=(self.ENTRY.start_date - datetime.timedelta(days=2)),
                                end_date=(self.ENTRY.end_date - datetime.timedelta(days=2)),
                                num_pass=1,
                                num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ]))

    def test_obeys_unreliable_min_runs(self):
        """
        Tests that update_tags() only considers a test unreliable if it has more than
        'unreliable_min_runs'.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1),
            unreliable_min_runs=100)

        initial_tags = collections.OrderedDict()
        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, initial_tags)

    def test_obeys_unreliable_time_period(self):
        """
        Tests that update_tags() ignores failures from before 'unreliable_time_period'.
        """

        config = self.CONFIG._replace(
            test_fail_rates=self.CONFIG.test_fail_rates._replace(unacceptable=0.1),
            task_fail_rates=self.CONFIG.task_fail_rates._replace(unacceptable=0.1),
            variant_fail_rates=self.CONFIG.variant_fail_rates._replace(unacceptable=0.1),
            distro_fail_rates=self.CONFIG.distro_fail_rates._replace(unacceptable=0.1))

        initial_tags = collections.OrderedDict([
            ("jstests/core/all.js", [
                "unreliable",
                "unreliable|jsCore_WT",
                "unreliable|jsCore_WT|linux-64",
                "unreliable|jsCore_WT|linux-64|rhel62",
            ]),
        ])

        lifecycle = ci_tags.TagsConfig.from_dict(
            dict(selector=dict(js_test=copy.deepcopy(initial_tags))))
        summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        tests = ["jstests/core/all.js"]
        report = test_failures.Report([
            self.ENTRY._replace(start_date=(self.ENTRY.start_date - datetime.timedelta(days=1)),
                                end_date=(self.ENTRY.end_date - datetime.timedelta(days=1)),
                                num_pass=0,
                                num_fail=1),
            self.ENTRY._replace(start_date=(self.ENTRY.start_date - datetime.timedelta(days=2)),
                                end_date=(self.ENTRY.end_date - datetime.timedelta(days=2)),
                                num_pass=0,
                                num_fail=1),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0, task="jsCore"),
            self.ENTRY._replace(num_pass=1, num_fail=0, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(summary_lifecycle, config, report, tests)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, collections.OrderedDict())


class TestCombinationHelpers(unittest.TestCase):
    def test_from_entry(self):
        entry = test_failures._ReportEntry(
            "testA", "taskA", "variantA", "distroA",
            datetime.date.today(),
            datetime.date.today(), 0, 0)
        combination = update_test_lifecycle._test_combination_from_entry(
            entry, test_failures.Report.TEST)
        self.assertEqual(combination, ("testA",))

        combination = update_test_lifecycle._test_combination_from_entry(
            entry, test_failures.Report.TEST_TASK)
        self.assertEqual(combination, ("testA", "taskA"))

        combination = update_test_lifecycle._test_combination_from_entry(
            entry, test_failures.Report.TEST_TASK_VARIANT)
        self.assertEqual(combination, ("testA", "taskA", "variantA"))

        combination = update_test_lifecycle._test_combination_from_entry(
            entry, test_failures.Report.TEST_TASK_VARIANT_DISTRO)
        self.assertEqual(combination, ("testA", "taskA", "variantA", "distroA"))

    def test_make_from_tag(self):
        test = "testA"

        combination = update_test_lifecycle._test_combination_from_tag(
            test, "unreliable")
        self.assertEqual(combination, ("testA",))

        combination = update_test_lifecycle._test_combination_from_tag(
            test, "unreliable|taskA")
        self.assertEqual(combination, ("testA", "taskA"))

        combination = update_test_lifecycle._test_combination_from_tag(
            test, "unreliable|taskA|variantA")
        self.assertEqual(combination, ("testA", "taskA", "variantA"))

        combination = update_test_lifecycle._test_combination_from_tag(
            test, "unreliable|taskA|variantA|distroA")
        self.assertEqual(combination, ("testA", "taskA", "variantA", "distroA"))


class TestCleanUpTags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evg = MockEvergreenConfig(["task1", "task2", "task3"],
                                      {"variant1": {"tasks": ["task1", "task2"],
                                                    "distros": ["distro1"]},
                                       "variant2": {"tasks": ["task3"],
                                                    "distros": ["distro2"]}})

    def test_is_unreliable_tag_relevant(self):
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(self.evg, "unreliable"))

    def test_is_unknown_task_relevant(self):
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task_unknown"))

    def test_is_known_task_relevant(self):
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1"))
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task2"))
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task3"))

    def test_is_unknown_variant_relevant(self):
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1|variant3"
        ))

    def test_is_unknown_task_variant_relevant(self):
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task3|variant1"))
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1|variant2"))

    def test_is_known_task_variant_relevant(self):
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1|variant1"))
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task2|variant1"))
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task3|variant2"))

    def test_is_unknown_task_variant_distro_relevant(self):
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1|variant1|distro2"))
        self.assertFalse(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task3|variant2|distro1"))

    def test_is_known_task_variant_distro_relevant(self):
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task1|variant1|distro1"))
        self.assertTrue(update_test_lifecycle._is_tag_still_relevant(
            self.evg, "unreliable|task3|variant2|distro2"))


class MockEvergreenConfig(object):
    def __init__(self, tasks, variants):
        self.task_names = tasks
        self.variants = {}
        for name, fields in variants.items():
            self.variants[name] = MockVariant(fields["tasks"], fields["distros"])

    def get_variant(self, variant_name):
        return self.variants.get(variant_name)


class MockVariant(object):
    def __init__(self, task_names, distros):
        self.task_names = task_names
        self.distros = distros


class TestJiraIssueCreator(unittest.TestCase):
    def test_description(self):
        data = {"js_test": {"testfile1": {"tag1": 0.1, "tag2": 0.2},
                            "testfile2": {"tag1": 0.1, "tag3": 0.3}}}
        desc = update_test_lifecycle.JiraIssueCreator._make_updated_tags_description(data)
        expected = ("- *js_test*\n"
                    "-- {{testfile1}}\n"
                    "--- {{tag1}} (0.10)\n"
                    "--- {{tag2}} (0.20)\n"
                    "-- {{testfile2}}\n"
                    "--- {{tag1}} (0.10)\n"
                    "--- {{tag3}} (0.30)")
        self.assertEqual(expected, desc)

    def test_description_empty(self):
        data = {}
        desc = update_test_lifecycle.JiraIssueCreator._make_updated_tags_description(data)
        expected = "_None_"
        self.assertEqual(expected, desc)

    def test_clean_up_description(self):
        data = {"js_test": {"testfile1": ["tag1", "tag2"],
                            "testfile2": []}}
        desc = update_test_lifecycle.JiraIssueCreator._make_tags_cleaned_up_description(data)
        expected = ("- *js_test*\n"
                    "-- {{testfile1}}\n"
                    "--- {{tag1}}\n"
                    "--- {{tag2}}\n"
                    "-- {{testfile2}}\n"
                    "--- ALL (test file removed or renamed as part of an earlier commit)")
        self.assertEqual(expected, desc)

    def test_clean_up_description_empty(self):
        data = {}
        desc = update_test_lifecycle.JiraIssueCreator._make_tags_cleaned_up_description(data)
        expected = "_None_"
        self.assertEqual(expected, desc)

    def test_truncate_description(self):
        desc = "a" * (update_test_lifecycle.JiraIssueCreator._MAX_DESCRIPTION_SIZE - 1)
        self.assertTrue(desc == update_test_lifecycle.JiraIssueCreator._truncate_description(desc))

        desc += "a"
        self.assertTrue(desc == update_test_lifecycle.JiraIssueCreator._truncate_description(desc))

        desc += "a"
        self.assertTrue(len(update_test_lifecycle.JiraIssueCreator._truncate_description(desc)) <=
                        update_test_lifecycle.JiraIssueCreator._MAX_DESCRIPTION_SIZE)


class TestTagsConfigWithChangelog(unittest.TestCase):
    def setUp(self):
        lifecycle = ci_tags.TagsConfig({"selector": {}})
        self.summary_lifecycle = update_test_lifecycle.TagsConfigWithChangelog(lifecycle)

    def test_add_tag(self):
        self.summary_lifecycle.add_tag("js_test", "testfile1", "tag1", 0.1)
        self.assertEqual({"js_test": {"testfile1": {"tag1": 0.1}}}, self.summary_lifecycle.added)

    def test_remove_tag(self):
        self.summary_lifecycle.lifecycle.add_tag("js_test", "testfile1", "tag1")
        self.summary_lifecycle.remove_tag("js_test", "testfile1", "tag1", 0.1)
        self.assertEqual({"js_test": {"testfile1": {"tag1": 0.1}}}, self.summary_lifecycle.removed)

    def test_add_remove_tag(self):
        self.summary_lifecycle.add_tag("js_test", "testfile1", "tag1", 0.1)
        self.summary_lifecycle.remove_tag("js_test", "testfile1", "tag1", 0.4)
        self.assertEqual({}, self.summary_lifecycle.added)
        self.assertEqual({}, self.summary_lifecycle.removed)

    def test_remove_add_tag(self):
        self.summary_lifecycle.lifecycle.add_tag("js_test", "testfile1", "tag1")
        self.summary_lifecycle.remove_tag("js_test", "testfile1", "tag1", 0.1)
        self.summary_lifecycle.add_tag("js_test", "testfile1", "tag1", 0.1)
        self.assertEqual({}, self.summary_lifecycle.added)
        self.assertEqual({}, self.summary_lifecycle.removed)
