"""Unittest for the resmokelib.testing.fixturelib.utils module"""

import copy
import unittest

from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib


class TestMergeMongoOptionDicts(unittest.TestCase):
    def setUp(self) -> None:
        self.under_test = FixtureLib()

    def test_merge_empty(self):
        original = {
            "dbpath": "value0", self.under_test.SET_PARAMETERS_KEY: {
                "param1": "value1",
                "param2": "value2",
            }
        }

        override = {}
        merged = self.under_test.merge_mongo_option_dicts(copy.deepcopy(original), override)

        self.assertDictEqual(merged, original)

    def test_merge_non_params(self):
        non_param1_key = "non_param1"
        non_param2_key = "non_param2"
        original = {
            non_param1_key: "value0", non_param2_key: {"nested_param1": "value0", },
            self.under_test.SET_PARAMETERS_KEY: {"param1": "value1", }
        }

        override = {
            non_param1_key: "value1",
            non_param2_key: "value1",
        }

        self.under_test.merge_mongo_option_dicts(original, override)

        expected = {
            non_param1_key: "value1", non_param2_key: "value1",
            self.under_test.SET_PARAMETERS_KEY: {"param1": "value1", }
        }
        self.assertEqual(original, expected)

    def test_merge_params(self):
        original = {
            "dbpath": "value", self.under_test.SET_PARAMETERS_KEY: {
                "param1": "value",
                "param2": {"param3": "value", },
            }
        }

        override = {self.under_test.SET_PARAMETERS_KEY: {"param2": {"param3": {"param4": "value"}}}}
        self.under_test.merge_mongo_option_dicts(original, override)

        expected = {
            "dbpath": "value", self.under_test.SET_PARAMETERS_KEY: {
                "param1": "value", "param2": {"param3": {"param4": "value"}}
            }
        }

        self.assertDictEqual(original, expected)
