import os
import sys
import unittest
from unittest.mock import patch
from mock import MagicMock
from mongo_tooling_metrics import client
from mongo_tooling_metrics.base_metrics import TopLevelMetrics
import buildscripts.scons as under_test

# pylint: disable=unused-argument

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("sys.argv", [
    'buildscripts/scons.py', "CC=/opt/mongodbtoolchain/v4/bin/gcc",
    "CXX=/opt/mongodbtoolchain/v4/bin/g++", "NINJA_PREFIX=test_success", "--ninja"
])
@patch("atexit.register")
class TestSconsAtExitMetricsCollection(unittest.TestCase):
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    def test_scons_at_exit_metrics_collection(self, mock_atexit_register):
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

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=False))
    def test_no_scons_at_exit_metrics_collection(self, mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=False))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    def test_scons_no_metrics_collection_non_vw(self, mock_atexit_register):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch("buildscripts.moduleconfig.get_module_sconscripts", MagicMock(side_effect=Exception()))
    def test_scons_at_exit_metrics_collection_exc(self, mock_atexit_register):
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
