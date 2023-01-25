import atexit
import os
import shutil
import unittest
from unittest.mock import patch
from mock import MagicMock
from mongo_tooling_metrics import client
from mongo_tooling_metrics.base_metrics import TopLevelMetrics
import buildscripts.scons as under_test

BUILD_DIR = os.path.join(os.getcwd(), 'build')
VARIANT_DIR = "test_scons"
ARTIFACT_DIR = os.path.join(BUILD_DIR, VARIANT_DIR)

# Remove variant dir if it already exists
if os.path.exists(ARTIFACT_DIR):
    shutil.rmtree(ARTIFACT_DIR)

ARGS = [
    'buildscripts/scons.py',
    f"VARIANT_DIR={VARIANT_DIR}",
    f"--build-dir={BUILD_DIR}",
    "--build-profile=fast",
    "--ninja=disabled",
    "install-platform",
]


@patch("sys.argv", ARGS)
@patch.object(TopLevelMetrics, 'should_collect_metrics', MagicMock(return_value=True))
class TestSconsAtExitMetricsCollection(unittest.TestCase):
    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch.object(atexit, "register", MagicMock())
    def test_at_exit_metrics_collection(self):
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
        assert len(metrics.build_info.build_artifacts) > 0
        assert metrics.command_info.command == ARGS
        assert metrics.command_info.positional_args == [
            f"VARIANT_DIR={VARIANT_DIR}", "install-platform"
        ]

    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=False))
    @patch.object(atexit, "register", MagicMock())
    def test_no_at_exit_metrics_collection(self):
        with self.assertRaises(SystemExit) as _:
            under_test.entrypoint()
        atexit_functions = [call[0][0].__name__ for call in atexit.register.call_args_list]
        assert "_verbosity_enforced_save_metrics" not in atexit_functions

    @patch.object(client, 'should_collect_internal_metrics', MagicMock(return_value=True))
    @patch("buildscripts.moduleconfig.get_module_sconscripts", MagicMock(side_effect=Exception()))
    @patch.object(atexit, "register", MagicMock())
    def test_at_exit_metrics_collection_exc(self):
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
        assert metrics.command_info.command == ARGS
        assert metrics.command_info.positional_args == [
            f"VARIANT_DIR={VARIANT_DIR}", "install-platform"
        ]
