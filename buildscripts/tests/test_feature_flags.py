"""Unit tests for feature_flag_tags_check.py."""

import unittest

import yaml

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_on_by_default


class TestFeatureFlags(unittest.TestCase):
    def test_default_on_flag_not_fully_disabled(self):
        with open(
            "buildscripts/resmokeconfig/fully_disabled_feature_flags.yml", encoding="utf8"
        ) as fully_disabled_ffs:
            fully_disabled_flags = yaml.safe_load(fully_disabled_ffs)

        default_on_flags = get_all_feature_flags_turned_on_by_default()

        for flag in default_on_flags:
            self.assertNotIn(
                flag,
                fully_disabled_flags,
                f"Feature flag {flag} defaults to true but is listed as fully disabled.",
            )
