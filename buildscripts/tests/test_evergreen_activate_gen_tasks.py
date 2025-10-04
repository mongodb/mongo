"""Unit tests for the generate_resmoke_suite script."""

import unittest

from mock import MagicMock, mock

from buildscripts import evergreen_activate_gen_tasks as under_test
from evergreen import EvergreenApi, Task, Version


def build_mock_task(display_name, task_id):
    mock_task = MagicMock(spec_set=Task, display_name=display_name, task_id=task_id)
    return mock_task


def build_mock_task_list(num_tasks):
    return [build_mock_task(f"task_{i}", f"id_{i}") for i in range(num_tasks)]


class MockVariantData:
    """An object to help create a mock evg api."""

    def __init__(self, build_id, variant_name, task_list):
        self.build_id = build_id
        self.variant_name = variant_name
        self.task_list = task_list


def build_mock_evg_api(variant_data_list):
    class VersionPatchedSpec(Version):
        """A patched `Version` with instance properties included for magic mock spec."""

        build_variants_map = MagicMock()

    mock_version = MagicMock(spec_set=VersionPatchedSpec)
    mock_version.build_variants_map = {
        variant_data.variant_name: variant_data.build_id for variant_data in variant_data_list
    }

    mock_evg_api = MagicMock(spec_set=EvergreenApi)
    mock_evg_api.version_by_id.return_value = mock_version

    build_id_mapping = {
        variant_data.build_id: variant_data.task_list for variant_data in variant_data_list
    }

    def tasks_by_build_side_effect(build_id):
        return build_id_mapping[build_id]

    mock_evg_api.tasks_by_build.side_effect = tasks_by_build_side_effect
    return mock_evg_api


class TestActivateTask(unittest.TestCase):
    def test_task_with_display_name_is_activated(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "task_3_gen",
            }
        )
        mock_task_list = build_mock_task_list(5)
        mock_evg_api = build_mock_evg_api(
            [MockVariantData("build_id", "non-burn-in-bv", mock_task_list)]
        )

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_called_with("id_3", activated=True)

    def test_task_with_no_matching_name(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "not_an_existing_task",
            }
        )
        mock_task_list = build_mock_task_list(5)
        mock_evg_api = build_mock_evg_api(
            [MockVariantData("build_id", "non-burn-in-bv", mock_task_list)]
        )

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_not_called()

    def test_burn_in_tags_tasks_are_activated(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "burn_in_tags_gen",
            }
        )
        mock_task_list_2 = build_mock_task_list(5)
        mock_task_list_2.append(build_mock_task("burn_in_tests", "burn_in_tests_id_2"))
        mock_task_list_3 = build_mock_task_list(5)
        mock_task_list_3.append(build_mock_task("burn_in_tests", "burn_in_tests_id_3"))
        mock_evg_api = build_mock_evg_api(
            [
                MockVariantData("1", "variant1-generated-by-burn-in-tags", mock_task_list_2),
                MockVariantData("2", "variant2-generated-by-burn-in-tags", mock_task_list_3),
            ]
        )

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_has_calls(
            [
                mock.call("burn_in_tests_id_2", activated=True),
                mock.call("burn_in_tests_id_3", activated=True),
            ]
        )

    def test_burn_in_tags_task_skips_non_existing_build_variant(self):
        expansions = under_test.EvgExpansions(
            **{
                "build_id": "build_id",
                "version_id": "version_id",
                "task_name": "burn_in_tags_gen",
            }
        )
        mock_task_list_1 = build_mock_task_list(5)
        mock_task_list_1.append(build_mock_task("burn_in_tags_gen", "burn_in_tags_gen_id_1"))
        mock_task_list_2 = build_mock_task_list(5)
        mock_task_list_2.append(build_mock_task("burn_in_tests", "burn_in_tests_id_2"))
        mock_evg_api = build_mock_evg_api(
            [
                MockVariantData("1", "variant1-non-burn-in", mock_task_list_1),
                MockVariantData("2", "variant2-generated-by-burn-in-tags", mock_task_list_2),
            ]
        )

        under_test.activate_task(expansions, mock_evg_api)

        mock_evg_api.configure_task.assert_called_once_with("burn_in_tests_id_2", activated=True)
