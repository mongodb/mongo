from datetime import datetime
import os
import sys
import unittest
from unittest.mock import patch

import buildscripts.resmoke as under_test

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
CURRENT_DATE_TIME = datetime(2022, 10, 4)

# pylint: disable=unused-argument

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("buildscripts.resmokelib.logging.flush._FLUSH_THREAD", None)
@patch("atexit.register")
class TestResmokeAtExitMetricsCollection(unittest.TestCase):
    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=True)
    def test_resmoke_at_exit_metrics_collection(self, mock_should_collect_metrics,
                                                mock_atexit_register):
        under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" in atexit_functions

    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=False)
    def test_no_resmoke_at_exit_metrics_collection(self, mock_should_collect_metrics,
                                                   mock_atexit_register):
        under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" not in atexit_functions

    @patch("sys.argv", ['buildscripts/resmoke.py', 'run', '--suite', 'buildscripts_test'])
    @patch("buildscripts.metrics.tooling_metrics_utils._should_collect_metrics", return_value=True)
    @patch("buildscripts.resmokelib.testing.executor.TestSuiteExecutor._run_tests",
           side_effect=Exception())
    def test_resmoke_at_exit_metrics_collection_exc(
            self, mock_exc_method, mock_should_collect_metrics, mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_metrics" in atexit_functions
