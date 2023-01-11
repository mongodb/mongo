import atexit
import os
import sys
import unittest
from unittest.mock import patch
from mock import MagicMock
from mongo_tooling_metrics import client
from mongo_tooling_metrics.lib.utils import _is_virtual_workstation
from mongo_tooling_metrics.base_metrics import TopLevelMetrics
import buildscripts.scons as under_test

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


class InvalidSconsConfiguration(Exception):
    """Exception raised if the scons invocation itself fails."""
    pass


@patch("sys.argv", [
    'buildscripts/scons.py', "CC=/opt/mongodbtoolchain/v4/bin/gcc",
    "CXX=/opt/mongodbtoolchain/v4/bin/g++", "NINJA_PREFIX=test_success", "--ninja"
])
class TestSconsAtExitMetricsCollection(unittest.TestCase):
    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(atexit, "register", MagicMock())
    def at_exit_metrics_collection(self):
        with self.assertRaises(SystemExit) as exc_info:
            under_test.entrypoint()

        if exc_info.exception.code != 0:
            raise InvalidSconsConfiguration("This SCons invocation is not supported on this host.")

        atexit_functions = [
            call for call in atexit.register.call_args_list
            if call[0][0].__name__ == '_verbosity_enforced_save_metrics'
        ]
        generate_metrics = atexit_functions[0][0][1].generate_metrics
        kwargs = atexit_functions[0][1]
        metrics = generate_metrics(**kwargs)

        assert not metrics.is_malformed()

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=False))
    @patch.object(atexit, "register", MagicMock())
    def no_at_exit_metrics_collection(self):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in atexit.register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=False))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(atexit, "register", MagicMock())
    def no_metrics_collection_non_vw(self):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in atexit.register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch("buildscripts.moduleconfig.get_module_sconscripts", MagicMock(side_effect=Exception()))
    @patch.object(atexit, "register", MagicMock())
    def at_exit_metrics_collection_exc(self):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()

        atexit_functions = [
            call for call in atexit.register.call_args_list
            if call[0][0].__name__ == '_verbosity_enforced_save_metrics'
        ]
        generate_metrics = atexit_functions[0][0][1].generate_metrics
        kwargs = atexit_functions[0][1]
        metrics = generate_metrics(**kwargs)

        assert not metrics.is_malformed()

    def test_scons_metrics_collection_at_exit(self):
        """Run all tests in this TestCase sequentially from this method."""

        try:
            # If this test fails and this is NOT a Virtual Workstation, we bail because metrics
            # collection is only supported on virtual workstations
            self.at_exit_metrics_collection()
        except InvalidSconsConfiguration:
            if not _is_virtual_workstation():
                return
            raise InvalidSconsConfiguration
        self.no_at_exit_metrics_collection()
        self.no_metrics_collection_non_vw()
        self.at_exit_metrics_collection_exc()
