"""Unit tests for the util/read_config.py file."""

import unittest
import mock

from buildscripts.util import read_config

# pylint: disable=missing-docstring,protected-access


class TestGetConfigValue(unittest.TestCase):
    def test_undefined_values_return_none(self):
        value = read_config.get_config_value("unknown", {}, {})

        self.assertEqual(None, value)

    def test_default_can_be_specified(self):
        value = read_config.get_config_value("option", {}, {}, default="default")

        self.assertEqual("default", value)

    def test_exception_throw_for_missing_required(self):
        self.assertRaises(KeyError, read_config.get_config_value, "missing", {}, {}, required=True)

    def test_config_file_value_is_used(self):
        value = read_config.get_config_value("option", {}, {"option": "value 0"}, default="default",
                                             required=True)
        self.assertEqual("value 0", value)

    def test_cmdline_value_is_used(self):
        cmdline_mock = mock.Mock
        cmdline_mock.option = "cmdline value"
        value = read_config.get_config_value("option", cmdline_mock, {"option": "value 0"},
                                             default="default", required=True)

        self.assertEqual("cmdline value", value)
