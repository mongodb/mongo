"""Unit tests for configure_resmoke's handling of --enableEvergreenApiTestSelection."""

import unittest
from unittest.mock import patch

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib.parser import parse, parse_command_line


class TestEnableEvergreenApiTestSelection(unittest.TestCase):
    """Test selection is off unless the flag explicitly turns it on.

    Whether the Evergreen project allows test selection is decided in
    evergreen/resmoke_tests_execute.sh, which passes the flag along; resmoke itself only
    honors what it is given.
    """

    def setUp(self):
        self._orig_tss_enabled = _config.ENABLE_EVERGREEN_API_TEST_SELECTION

    def tearDown(self):
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = self._orig_tss_enabled

    def test_defaults_to_disabled(self):
        self.assertIs(_config.DEFAULTS["enable_evergreen_api_test_selection"], False)

    # --- flag parsing ---

    def test_flag_omitted_parses_as_none(self):
        # None is what lets _update_config_vars() fall through to DEFAULTS.
        _, parsed_args = parse(["run", "--suite=my_suite"])
        self.assertIsNone(parsed_args["enable_evergreen_api_test_selection"])

    def test_flag_value_is_case_insensitive(self):
        for value in ("true", "True", "TRUE"):
            with self.subTest(value=value):
                _, parsed_args = parse(
                    ["run", "--suite=my_suite", f"--enableEvergreenApiTestSelection={value}"]
                )
                self.assertIs(parsed_args["enable_evergreen_api_test_selection"], True)

    def test_non_true_flag_value_parses_as_false(self):
        for value in ("false", "False", "no"):
            with self.subTest(value=value):
                _, parsed_args = parse(
                    ["run", "--suite=my_suite", f"--enableEvergreenApiTestSelection={value}"]
                )
                self.assertIs(parsed_args["enable_evergreen_api_test_selection"], False)

    # --- end-to-end into _config ---

    @patch("pathlib.Path.exists", return_value=True)
    def test_flag_omitted_leaves_test_selection_disabled(self, mock_exists):
        parse_command_line(["run", "--suite=my_suite", "my_test.js"], should_configure_otel=False)
        self.assertIs(_config.ENABLE_EVERGREEN_API_TEST_SELECTION, False)

    @patch("pathlib.Path.exists", return_value=True)
    def test_flag_true_enables_test_selection(self, mock_exists):
        parse_command_line(
            [
                "run",
                "--suite=my_suite",
                "--enableEvergreenApiTestSelection=true",
                "my_test.js",
            ],
            should_configure_otel=False,
        )
        self.assertIs(_config.ENABLE_EVERGREEN_API_TEST_SELECTION, True)

    @patch("pathlib.Path.exists", return_value=True)
    def test_flag_false_overrides_enabled_state(self, mock_exists):
        # Explicit false must not be mistaken for "unset" when merging over DEFAULTS.
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        parse_command_line(
            [
                "run",
                "--suite=my_suite",
                "--enableEvergreenApiTestSelection=false",
                "my_test.js",
            ],
            should_configure_otel=False,
        )
        self.assertIs(_config.ENABLE_EVERGREEN_API_TEST_SELECTION, False)


if __name__ == "__main__":
    unittest.main()
