"""Unit tests for the generate_resmoke_suite script."""
# pylint: disable=invalid-name
import unittest

from mock import MagicMock, mock

from buildscripts import evergreen_activate_gen_tasks as under_test
from evergreen import Build, EvergreenApi, Task, Version


def build_mock_task(display_name, task_id):
    mock_task = MagicMock(spec_set=Task, display_name=display_name, task_id=task_id)
    return mock_task


def build_mock_task_list(num_tasks):
    return [build_mock_task(f"task_{i}", f"id_{i}") for i in range(num_tasks)]


def build_mock_build(mock_task_list):
    mock_build = MagicMock(spec_set=Build)
    mock_build.get_tasks.return_value = mock_task_list
    return mock_build


def build_mock_evg_api(mock_current_build, mock_other_builds_list):
    mock_version = MagicMock(spec_set=Version)
    mock_version.build_by_variant.side_effect = mock_other_builds_list
    mock_evg_api = MagicMock(spec_set=EvergreenApi)
    mock_evg_api.version_by_id.return_value = mock_version
    mock_evg_api.build_by_id.return_value = mock_current_build
    return mock_evg_api


class TestActivateTask(unittest.TestCase):
    def test_task_with_display_name_is_activated(self):
        expansions = under_test.EvgExpansions(**{
            "build_id": "build_id",
            "version_id": "version_id",
            "task_name": "task_3_gen",
        })
        mock_task_list = build_mock_task_list(5)
        mock_evg_api = build_mock_evg_api(build_mock_build(mock_task_list), [])

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_called_with("id_3", activated=True)

    def test_task_with_no_matching_name(self):
        expansions = under_test.EvgExpansions(**{
            "build_id": "build_id",
            "version_id": "version_id",
            "task_name": "not_an_existing_task",
        })
        mock_task_list = build_mock_task_list(5)
        mock_evg_api = build_mock_evg_api(build_mock_build(mock_task_list), [])

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_not_called()

    def test_burn_in_tags_tasks_are_activated(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "burn_in_tags_gen",
                "burn_in_tag_buildvariants": "build_variant_2 build_variant_3",
            })
        mock_task_list_1 = build_mock_task_list(5)
        mock_task_list_1.append(build_mock_task("burn_in_tags_gen", "burn_in_tags_gen_id_1"))
        mock_task_list_2 = build_mock_task_list(5)
        mock_task_list_2.append(build_mock_task("burn_in_tests", "burn_in_tests_id_2"))
        mock_task_list_3 = build_mock_task_list(5)
        mock_task_list_3.append(build_mock_task("burn_in_tests", "burn_in_tests_id_3"))
        mock_evg_api = build_mock_evg_api(
            build_mock_build(mock_task_list_1), [
                build_mock_build(mock_task_list_2),
                build_mock_build(mock_task_list_3),
            ])

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_has_calls([
            mock.call("burn_in_tests_id_2", activated=True),
            mock.call("burn_in_tests_id_3", activated=True)
        ])

    def test_burn_in_tags_task_skips_non_existing_build_variant(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "burn_in_tags_gen",
                "burn_in_tag_buildvariants": "not_an_existing_build_variant build_variant_2",
            })
        mock_task_list_1 = build_mock_task_list(5)
        mock_task_list_1.append(build_mock_task("burn_in_tags_gen", "burn_in_tags_gen_id_1"))
        mock_task_list_2 = build_mock_task_list(5)
        mock_task_list_2.append(build_mock_task("burn_in_tests", "burn_in_tests_id_2"))
        mock_evg_api = build_mock_evg_api(
            build_mock_build(mock_task_list_1), [
                KeyError,
                build_mock_build(mock_task_list_2),
            ])

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_called_once_with("burn_in_tests_id_2", activated=True)

    def test_burn_in_tags_task_with_missing_burn_in_tag_buildvariants_expansion(self):
        expansions = under_test.EvgExpansions(**{
            "build_id": "build_id",
            "version_id": "version_id",
            "task_name": "burn_in_tags_gen",
        })
        mock_task_list_1 = build_mock_task_list(5)
        mock_task_list_1.append(build_mock_task("burn_in_tags_gen", "burn_in_tags_gen_id_1"))
        mock_evg_api = build_mock_evg_api(build_mock_build(mock_task_list_1), [])

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_not_called()
