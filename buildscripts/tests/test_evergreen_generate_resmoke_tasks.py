"""Unit tests for the generate_resmoke_suite script."""
from datetime import datetime, timedelta
import json
import os
from tempfile import TemporaryDirectory
import sys
import unittest

import inject
import requests
import yaml
from mock import patch, MagicMock
from evergreen import EvergreenApi

from buildscripts import evergreen_generate_resmoke_tasks as under_test
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, FallbackStrategy, \
    greedy_division, round_robin_fallback
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access
# pylint: disable=too-many-locals,too-many-lines,too-many-public-methods,no-value-for-parameter


def tst_stat_mock(file, duration, pass_count):
    return MagicMock(test_file=file, avg_duration_pass=duration, num_pass=pass_count)


def mock_test_stats_unavailable(evg_api_mock):
    response = MagicMock(status_code=requests.codes.SERVICE_UNAVAILABLE)
    evg_api_mock.test_stats_by_project.side_effect = requests.HTTPError(response=response)
    return evg_api_mock


def mock_resmoke_config_file(test_list, filename):
    config = {
        "test_kind": "js_test",
        "selector": {
            "roots": test_list,
        },
        "executor": {
            "config": {
                "shell_options": {
                    "global_vars": {
                        "TestData": {
                            "roleGraphInvalidationIsFatal": True,
                        }
                    }
                }
            }
        }
    }  # yapf: disable

    with open(filename, "w") as fileh:
        fileh.write(yaml.safe_dump(config))


def configure_dependencies(evg_api, evg_expansions, config_dir,
                           test_suites_dir=under_test.DEFAULT_TEST_SUITE_DIR):
    start_date = datetime.utcnow()
    end_date = start_date - timedelta(weeks=2)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(SuiteSplitConfig, evg_expansions.get_suite_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.get_evg_config_gen_options(config_dir))
        binder.bind(EvergreenApi, evg_api)
        binder.bind(GenerationConfiguration,
                    GenerationConfiguration.from_yaml_file(under_test.GENERATE_CONFIG_FILE))
        binder.bind(ResmokeProxyConfig, ResmokeProxyConfig(resmoke_suite_dir=test_suites_dir))

    inject.clear_and_configure(dependencies)


def build_mock_evg_expansions(target_resmoke_time=under_test.DEFAULT_TARGET_RESMOKE_TIME):
    return under_test.EvgExpansions(
        build_variant="build_variant",
        max_sub_suites=100,
        project="mongodb-mongo-master",
        task_id="task314",
        task_name="some_task_gen",
        target_resmoke_time=target_resmoke_time,
        build_id="build_id",
        revision="abc123",
    )


class TestAcceptance(unittest.TestCase):
    """A suite of Acceptance tests for evergreen_generate_resmoke_tasks."""

    @staticmethod
    def _mock_evg_api(successful_task=False):
        evg_api_mock = MagicMock()
        task_mock = evg_api_mock.task_by_id.return_value
        task_mock.execution = 0
        if successful_task:
            task_mock.execution = 1
            task_mock.get_execution.return_value.is_success.return_value = True

        return evg_api_mock

    @staticmethod
    def _prep_dirs(tmpdir):
        target_directory = os.path.join(tmpdir, "output")
        source_directory = os.path.join(tmpdir, "input")
        os.makedirs(source_directory)

        return target_directory, source_directory

    @staticmethod
    def _mock_test_files(directory, n_tests, runtime, evg_api_mock, suites_config_mock):
        test_list = [os.path.join(directory, f"test_name_{i}.js") for i in range(n_tests)]
        mock_test_stats = [tst_stat_mock(file, runtime, 5) for file in test_list]
        evg_api_mock.test_stats_by_project.return_value = mock_test_stats
        suites_config_mock.return_value.tests = test_list
        for test in test_list:
            open(test, "w").close()

        return test_list

    def test_when_task_has_already_run_successfully(self):
        """
        Given evergreen_generate_resmoke_tasks has already been run successfully by this task,
        When it attempts to run again,
        It does not generate any files.
        """
        mock_evg_api = self._mock_evg_api(successful_task=True)
        mock_evg_expansions = build_mock_evg_expansions()

        with TemporaryDirectory() as tmpdir:
            configure_dependencies(mock_evg_api, mock_evg_expansions, tmpdir)

            orchestrator = under_test.EvgGenResmokeTaskOrchestrator()
            orchestrator.generate_task(mock_evg_expansions.task_id,
                                       mock_evg_expansions.get_suite_split_params(),
                                       mock_evg_expansions.get_gen_params())

            self.assertEqual(0, len(os.listdir(tmpdir)))

    @patch("buildscripts.resmokelib.suitesconfig.get_suite")
    def test_when_evg_test_stats_is_down(self, suites_config_mock):
        """
        Given Evergreen historic test stats endpoint is disabled,
        When evergreen_generate_resmoke_tasks attempts to generate suites,
        It generates suites based on "fallback_num_sub_suites".
        """
        n_tests = 100
        mock_evg_api = mock_test_stats_unavailable(self._mock_evg_api())
        mock_evg_expansions = build_mock_evg_expansions()
        task = mock_evg_expansions.task_name[:-4]

        with TemporaryDirectory() as tmpdir:
            target_directory, source_directory = self._prep_dirs(tmpdir)
            configure_dependencies(mock_evg_api, mock_evg_expansions, target_directory,
                                   source_directory)

            suite_path = os.path.join(source_directory, task)
            test_list = self._mock_test_files(source_directory, n_tests, 5, mock_evg_api,
                                              suites_config_mock)
            mock_resmoke_config_file(test_list, suite_path + ".yml")

            orchestrator = under_test.EvgGenResmokeTaskOrchestrator()
            orchestrator.generate_task(mock_evg_expansions.task_id,
                                       mock_evg_expansions.get_suite_split_params(),
                                       mock_evg_expansions.get_gen_params())

            # Were all the config files created? There should be one for each suite as well as
            # the evergreen json config.
            generated_files = os.listdir(target_directory)
            # The expected suite count is the number of fallback suites + the _misc suite.
            expected_suite_count = mock_evg_expansions.max_sub_suites + 1
            # We expect files for all the suites + the evergreen json config.
            self.assertEqual(expected_suite_count + 1, len(generated_files))

            # Taking a closer look at the evergreen json config.
            expected_shrub_file = f"{task}.json"
            self.assertIn(expected_shrub_file, generated_files)
            with open(os.path.join(target_directory, expected_shrub_file)) as fileh:
                shrub_config = json.load(fileh)

                # Is there a task in the config for all the suites we created?
                self.assertEqual(expected_suite_count, len(shrub_config["tasks"]))

    @unittest.skipIf(
        sys.platform.startswith("win"), "Since this test is messing with directories, "
        "windows does not handle test generation correctly")
    @patch("buildscripts.resmokelib.suitesconfig.get_suite")
    def test_with_each_test_in_own_task(self, suites_config_mock):
        """
        Given a task with all tests having a historic runtime over the target,
        When evergreen_generate_resmoke_tasks attempts to generate suites,
        It generates a suite for each test.
        """
        n_tests = 4
        mock_evg_api = self._mock_evg_api()
        mock_evg_expansions = build_mock_evg_expansions(target_resmoke_time=10)
        task = mock_evg_expansions.task_name[:-4]

        with TemporaryDirectory() as tmpdir:
            target_directory, source_directory = self._prep_dirs(tmpdir)
            configure_dependencies(mock_evg_api, mock_evg_expansions, target_directory,
                                   source_directory)
            suite_path = os.path.join(source_directory, task)
            test_list = self._mock_test_files(source_directory, n_tests, 15 * 60, mock_evg_api,
                                              suites_config_mock)
            mock_resmoke_config_file(test_list, suite_path + ".yml")

            orchestrator = under_test.EvgGenResmokeTaskOrchestrator()
            orchestrator.generate_task(mock_evg_expansions.task_id,
                                       mock_evg_expansions.get_suite_split_params(),
                                       mock_evg_expansions.get_gen_params())

            # Were all the config files created? There should be one for each suite as well as
            # the evergreen json config.
            generated_files = os.listdir(target_directory)
            # The expected suite count is the number of tests + the _misc suite.
            expected_suite_count = n_tests + 1
            # We expect files for all the suites + the evergreen json config.
            self.assertEqual(expected_suite_count + 1, len(generated_files))

            # Taking a closer look at the evergreen json config.
            expected_shrub_file = f"{task}.json"
            self.assertIn(expected_shrub_file, generated_files)
            with open(os.path.join(target_directory, expected_shrub_file)) as fileh:
                shrub_config = json.load(fileh)

                # Is there a task in the config for all the suites we created?
                self.assertEqual(expected_suite_count, len(shrub_config["tasks"]))
