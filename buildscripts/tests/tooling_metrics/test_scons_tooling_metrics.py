from datetime import datetime
import os
import sys
import unittest
from unittest.mock import MagicMock, patch
import mongomock
import pymongo
import buildscripts.metrics.scons_tooling_metrics as under_test
from buildscripts.scons import entrypoint as scons_entrypoint

TEST_INTERNAL_TOOLING_METRICS_HOSTNAME = 'mongodb://testing:27017'
CURRENT_DATE_TIME = datetime(2022, 10, 4)

# pylint: disable=unused-argument
# pylint: disable=protected-access

# Metrics collection is not supported for Windows
if os.name == "nt":
    sys.exit()


@patch("sys.argv", [
    'buildscripts/scons.py', "CC=/opt/mongodbtoolchain/v3/bin/gcc",
    "CXX=/opt/mongodbtoolchain/v3/bin/g++", "NINJA_PREFIX=test_success", "--ninja"
])
@patch("buildscripts.metrics.scons_tooling_metrics.is_virtual_workstation", return_value=True)
@patch("atexit.register")
class TestSconsAtExitMetricsCollection(unittest.TestCase):
    def test_scons_at_exit_metrics_collection(self, mock_atexit_register,
                                              mock_is_virtual_workstation):
        with self.assertRaises(SystemExit) as context:
            scons_entrypoint()
        assert context.exception.code == 0
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_scons_tooling_metrics" in atexit_functions

    @patch("buildscripts.moduleconfig.get_module_sconscripts", side_effect=Exception())
    def test_scons_at_exit_metrics_collection_exc(self, mock_method, mock_atexit_register,
                                                  mock_is_virtual_workstation):
        with self.assertRaises(SystemExit) as context:
            scons_entrypoint()
        assert context.exception.code == 2
        atexit_functions = [call[0][0].__name__ for call in mock_atexit_register.call_args_list]
        assert "_save_scons_tooling_metrics" in atexit_functions


@patch("buildscripts.metrics.tooling_metrics_utils.INTERNAL_TOOLING_METRICS_HOSTNAME",
       TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
@patch("buildscripts.metrics.scons_tooling_metrics.is_virtual_workstation", return_value=True)
class TestSconsMetricsPersist(unittest.TestCase):
    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    def test_scons_metrics_collection_success(self, mock_is_virtual_workstation):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        under_test._save_scons_tooling_metrics(CURRENT_DATE_TIME, None, None, None, None,
                                               MagicMock(exit_code=0))
        assert client.metrics.tooling_metrics.find_one()

    @mongomock.patch(servers=((TEST_INTERNAL_TOOLING_METRICS_HOSTNAME), ))
    def test_scons_metrics_collection_fail(self, mock_is_virtual_workstation):
        client = pymongo.MongoClient(host=TEST_INTERNAL_TOOLING_METRICS_HOSTNAME)
        assert not client.metrics.tooling_metrics.find_one()
        under_test._save_scons_tooling_metrics(None, None, None, None, None, None)
        assert not client.metrics.tooling_metrics.find_one()
