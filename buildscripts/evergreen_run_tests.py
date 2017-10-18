#!/usr/bin/env python

"""
Command line utility for executing MongoDB tests in Evergreen.
"""

from __future__ import absolute_import

import collections
import os.path
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts import resmoke
from buildscripts import resmokelib


_TagInfo = collections.namedtuple("_TagInfo", ["tag_name", "evergreen_aware", "suite_options"])


class Main(resmoke.Main):
    """
    A class for executing potentially multiple resmoke.py test suites in a way that handles
    additional options for running unreliable tests in Evergreen.
    """

    UNRELIABLE_TAG = _TagInfo(tag_name="unreliable",
                              evergreen_aware=True,
                              suite_options=resmokelib.config.SuiteOptions.ALL_INHERITED._replace(
                                  report_failure_status="silentfail"))

    RESOURCE_INTENSIVE_TAG = _TagInfo(
        tag_name="resource_intensive",
        evergreen_aware=False,
        suite_options=resmokelib.config.SuiteOptions.ALL_INHERITED._replace(num_jobs=1))

    RETRY_ON_FAILURE_TAG = _TagInfo(
        tag_name="retry_on_failure",
        evergreen_aware=True,
        suite_options=resmokelib.config.SuiteOptions.ALL_INHERITED._replace(
            fail_fast=False,
            num_repeats=2,
            report_failure_status="silentfail"))

    def _make_evergreen_aware_tags(self, tag_name):
        """
        Returns a list of resmoke.py tags for task, variant, and distro combinations in Evergreen.
        """

        tags_format = ["{tag_name}"]

        if resmokelib.config.EVERGREEN_TASK_NAME is not None:
            tags_format.append("{tag_name}|{task_name}")

            if resmokelib.config.EVERGREEN_VARIANT_NAME is not None:
                tags_format.append("{tag_name}|{task_name}|{variant_name}")

                if resmokelib.config.EVERGREEN_DISTRO_ID is not None:
                    tags_format.append("{tag_name}|{task_name}|{variant_name}|{distro_id}")

        return [tag.format(tag_name=tag_name,
                           task_name=resmokelib.config.EVERGREEN_TASK_NAME,
                           variant_name=resmokelib.config.EVERGREEN_VARIANT_NAME,
                           distro_id=resmokelib.config.EVERGREEN_DISTRO_ID)
                for tag in tags_format]

    @classmethod
    def _make_tag_combinations(cls):
        """
        Returns a list of (tag, enabled) pairs representing all possible combinations of all
        possible pairings of whether the tags are enabled or disabled together.
        """

        combinations = []

        if resmokelib.config.EVERGREEN_PATCH_BUILD:
            combinations.append((
                "unreliable and resource intensive",
                ((cls.UNRELIABLE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append((
                "unreliable and not resource intensive",
                ((cls.UNRELIABLE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, False))))
            combinations.append((
                "reliable and resource intensive",
                ((cls.UNRELIABLE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append((
                "reliable and not resource intensive",
                ((cls.UNRELIABLE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG, False))))
        else:
            combinations.append((
                "retry on failure and resource intensive",
                ((cls.RETRY_ON_FAILURE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append((
                "retry on failure and not resource intensive",
                ((cls.RETRY_ON_FAILURE_TAG, True), (cls.RESOURCE_INTENSIVE_TAG, False))))
            combinations.append((
                "run once and resource intensive",
                ((cls.RETRY_ON_FAILURE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG, True))))
            combinations.append((
                "run once and not resource intensive",
                ((cls.RETRY_ON_FAILURE_TAG, False), (cls.RESOURCE_INTENSIVE_TAG, False))))

        return combinations

    def _get_suites(self):
        """
        Returns a list of resmokelib.testing.suite.Suite instances to execute.

        For every resmokelib.testing.suite.Suite instance returned by resmoke.Main._get_suites(),
        multiple copies of that test suite are run using different resmokelib.config.SuiteOptions()
        depending on whether each tag in the combination is enabled or not.
        """

        suites = []

        for suite in resmoke.Main._get_suites(self):
            if suite.test_kind != "js_test":
                # Tags are only support for JavaScript tests, so we leave the test suite alone when
                # running any other kind of test.
                suites.append(suite)
                continue

            for (tag_desc, tag_combo) in self._make_tag_combinations():
                suite_options_list = []

                for (tag_info, enabled) in tag_combo:
                    if tag_info.evergreen_aware:
                        tags = self._make_evergreen_aware_tags(tag_info.tag_name)
                        include_tags = {"$anyOf": tags}
                    else:
                        include_tags = tag_info.tag_name

                    if enabled:
                        suite_options = tag_info.suite_options._replace(include_tags=include_tags)
                    else:
                        suite_options = resmokelib.config.SuiteOptions.ALL_INHERITED._replace(
                            include_tags={"$not": include_tags})

                    suite_options_list.append(suite_options)

                suite_options = resmokelib.config.SuiteOptions.combine(*suite_options_list)
                suite_options = suite_options._replace(description=tag_desc)
                suites.append(suite.with_options(suite_options))

        return suites


if __name__ == "__main__":
    Main().run()
