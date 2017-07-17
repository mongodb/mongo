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
        self.assertEqual(collections.OrderedDict(), self.assert_has_only_js_tests(lifecycle))

        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(lifecycle, config, report)
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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        report = test_failures.Report([
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0, task="jsCore"),
            self.ENTRY._replace(num_pass=1, num_fail=0, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=1, num_fail=0, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(lifecycle, config, report)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, collections.OrderedDict())

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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        report = test_failures.Report([
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=1, num_fail=0, task="jsCore"),
            self.ENTRY._replace(num_pass=1, num_fail=0, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=1, num_fail=0, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(lifecycle, config, report)
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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(lifecycle, config, report)
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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

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
        update_test_lifecycle.update_tags(lifecycle, config, report)
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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

        report = test_failures.Report([
            self.ENTRY._replace(num_pass=0, num_fail=1),
            self.ENTRY._replace(num_pass=0, num_fail=1, task="jsCore"),
            self.ENTRY._replace(num_pass=0, num_fail=1, variant="linux-64-debug"),
            self.ENTRY._replace(num_pass=1, num_fail=0),
            self.ENTRY._replace(num_pass=0, num_fail=1, distro="rhel55"),
        ])

        update_test_lifecycle.validate_config(config)
        update_test_lifecycle.update_tags(lifecycle, config, report)
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
        self.assertEqual(initial_tags, self.assert_has_only_js_tests(lifecycle))

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
        update_test_lifecycle.update_tags(lifecycle, config, report)
        updated_tags = self.assert_has_only_js_tests(lifecycle)
        self.assertEqual(updated_tags, collections.OrderedDict())
