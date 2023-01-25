from datetime import datetime
import unittest
from unittest.mock import patch
from mock import MagicMock
from mongo_tooling_metrics import client
from mongo_tooling_metrics.base_metrics import TopLevelMetrics

import buildscripts.resmoke as under_test

CURRENT_DATE_TIME = datetime(2022, 10, 4)

# pylint: disable=unused-argument


@patch("buildscripts.resmokelib.logging.flush._FLUSH_THREAD", None)
@patch("atexit.register")
class TestResmokeAtExitMetricsCollection(unittest.TestCase):
    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    def test_resmoke_at_exit_metrics_collection(self, mock_atexit_register):
        under_test.entrypoint()

        atexit_functions = [
            call for call in mock_atexit_register.call_args_list
            if call[0][0].__name__ == '_verbosity_enforced_save_metrics'
        ]
        generate_metrics = atexit_functions[0][0][1].generate_metrics
        kwargs = atexit_functions[0][1]
        metrics = generate_metrics(**kwargs)

        assert not metrics.is_malformed()
        assert metrics.command_info.command == ['buildscripts/resmoke.py', 'list-suites']
        assert metrics.command_info.options['command'] == 'list-suites'
        assert metrics.command_info.positional_args == []

    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=False))
    def test_no_resmoke_at_exit_metrics_collection(self, mock_atexit_register):
        under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch("sys.argv", ['buildscripts/resmoke.py', 'list-suites'])
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=False))
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    def test_resmoke_no_metric_collection_non_vw(self, mock_atexit_register):
        under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch("sys.argv", ['buildscripts/resmoke.py', 'run', '--suite', 'buildscripts_test'])
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch("buildscripts.resmokelib.testing.executor.TestSuiteExecutor._run_tests",
           side_effect=Exception())
    def test_resmoke_at_exit_metrics_collection_exc(self, mock_exc_method, mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()

        atexit_functions = [
            call for call in mock_atexit_register.call_args_list
            if call[0][0].__name__ == '_verbosity_enforced_save_metrics'
        ]
        generate_metrics = atexit_functions[0][0][1].generate_metrics
        kwargs = atexit_functions[0][1]
        metrics = generate_metrics(**kwargs)

        assert not metrics.is_malformed()
        assert metrics.command_info.command == [
            'buildscripts/resmoke.py', 'run', '--suite', 'buildscripts_test'
        ]
        assert metrics.command_info.options['command'] == 'run'
        assert metrics.command_info.positional_args == []
