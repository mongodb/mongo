import os
import sys
import unittest
from unittest.mock import patch
import buildscripts.scons as under_test

# pylint: disable=unused-argument
# pylint: disable=protected-access

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("sys.argv", [
    'buildscripts/scons.py', "CC=/opt/mongodbtoolchain/v4/bin/gcc",
    "CXX=/opt/mongodbtoolchain/v4/bin/g++", "NINJA_PREFIX=test_success", "--ninja"
])
@patch("atexit.register")
class TestSconsAtExitMetricsCollection(unittest.TestCase):
    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=True)
    def test_scons_at_exit_metrics_collection(self, mock_should_collect_metrics,
                                              mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" in atexit_functions

    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=False)
    def test_no_scons_at_exit_metrics_collection(self, mock_should_collect_metrics,
                                                 mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" not in atexit_functions

    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=True)
    @patch("buildscripts.moduleconfig.get_module_sconscripts", side_effect=Exception())
    def test_scons_at_exit_metrics_collection_exc(
            self, mock_exc_method, mock_should_collect_metrics, mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" in atexit_functions
