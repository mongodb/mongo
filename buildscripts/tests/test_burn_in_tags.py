"""Unit tests for the burn_in_tags.py script."""

import datetime
import os
import unittest
import mock

from mock import Mock

from shrub.config import Configuration

from buildscripts import burn_in_tags

import buildscripts.ciconfig.evergreen as _evergreen

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access

NS = "buildscripts.burn_in_tags"
TEST_FILE_PATH = os.path.join(os.path.dirname(__file__), "test_burn_in_tags_evergreen.yml")


def ns(relative_name):  # pylint: disable-invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def get_expansions_data():
    return {
            "branch_name": "fake_branch",
            "build_variant": "enterprise-rhel-62-64-bit",
            "check_evergreen": 2,
            "distro_id": "rhel62-small",
            "is_patch": "true",
            "max_revisions": 25,
            "repeat_tests_max": 1000,
            "repeat_tests_min": 2,
            "repeat_tests_secs": 600,
            "revision": "fake_sha",
            "project": "fake_project",
    }  # yapf: disable

def get_evergreen_config():
    return _evergreen.parse_evergreen_file(TEST_FILE_PATH, evergreen_binary=None)


class TestCreateEvgBuildVariantMap(unittest.TestCase):
    def test_create_evg_buildvariant_map(self):
        expansions_file_data = {
            "base_variants":
                "enterprise-rhel-62-64-bit-majority-read-concern-off enterprise-rhel-62-64-bit-inmem"
        }
        buildvariant_map = burn_in_tags._create_evg_buildvariant_map(expansions_file_data)
        expected_buildvariant_map = {
            "enterprise-rhel-62-64-bit-majority-read-concern-off":
                "enterprise-rhel-62-64-bit-majority-read-concern-off-required",
            "enterprise-rhel-62-64-bit-inmem":
                "enterprise-rhel-62-64-bit-inmem-required"
        }
        self.assertEqual(buildvariant_map, expected_buildvariant_map)

    def test_create_evg_buildvariant_map_no_base_variants(self):
        expansions_file_data = {"base_variants": ""}
        buildvariant_map = burn_in_tags._create_evg_buildvariant_map(expansions_file_data)
        self.assertEqual(buildvariant_map, {})


class TestGenerateEvgBuildVariants(unittest.TestCase):
    @mock.patch(ns("evergreen"))
    def test_generate_evg_buildvariant_one_base_variant(self, evergreen_mock):
        evergreen_mock.parse_evergreen_file.return_value = get_evergreen_config()
        base_variant = "enterprise-rhel-62-64-bit-inmem"
        expected_variant_data = get_evergreen_config().get_variant(base_variant)

        shrub_config = Configuration()
        burn_in_tags._generate_evg_buildvariant(shrub_config, base_variant,
                                                "enterprise-rhel-62-64-bit-inmem-required")
        generated_buildvariants = shrub_config.to_map()["buildvariants"]
        self.assertEqual(len(generated_buildvariants), 1)
        generated_build_variant = generated_buildvariants[0]
        self.assertEqual(generated_build_variant["name"], f"{expected_variant_data}-required")
        self.assertEqual(generated_build_variant["modules"], expected_variant_data.modules)
        self.assertEqual(generated_build_variant["expansions"], expected_variant_data.expansions)


class TestGenerateEvgTasks(unittest.TestCase):
    @mock.patch(ns("evergreen"))
    @mock.patch(ns("create_tests_by_task"))
    def test_generate_evg_tasks_no_tests_changed(self, create_tests_by_task_mock, evergreen_mock):
        evergreen_mock.parse_evergreen_file.return_value = get_evergreen_config()
        create_tests_by_task_mock.return_value = {}
        expansions_file_data = get_expansions_data()
        buildvariant_map = {
            "enterprise-rhel-62-64-bit-inmem": "enterprise-rhel-62-64-bit-inmem-required",
            "enterprise-rhel-62-64-bit-majority-read-concern-off":
                "enterprise-rhel-62-64-bit-majority-read-concern-off-required",
        }  # yapf: disable
        shrub_config = Configuration()
        evergreen_api = Mock()
        burn_in_tags._generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data,
                                         buildvariant_map)

        self.assertEqual(shrub_config.to_map(), {})

    @mock.patch(ns("evergreen"))
    @mock.patch(ns("create_tests_by_task"))
    def test_generate_evg_tasks_one_test_changed(self, create_tests_by_task_mock, evergreen_mock):
        evergreen_mock.parse_evergreen_file.return_value = get_evergreen_config()
        create_tests_by_task_mock.return_value = {
            "aggregation_mongos_passthrough": {
                "resmoke_args":
                    "--suites=aggregation_mongos_passthrough --storageEngine=wiredTiger",
                "tests": ["jstests/aggregation/bugs/ifnull.js"],
                "use_multiversion": None
            }
        }  # yapf: disable
        expansions_file_data = get_expansions_data()
        buildvariant_map = {
            "enterprise-rhel-62-64-bit-inmem": "enterprise-rhel-62-64-bit-inmem-required",
            "enterprise-rhel-62-64-bit-majority-read-concern-off":
                "enterprise-rhel-62-64-bit-majority-read-concern-off-required",
        }  # yapf: disable
        shrub_config = Configuration()
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.return_value = [
            Mock(test_file="dir/test2.js", avg_duration_pass=10)
        ]
        burn_in_tags._generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data,
                                         buildvariant_map)

        generated_config = shrub_config.to_map()
        self.assertEqual(len(generated_config["buildvariants"]), 2)
        first_generated_build_variant = generated_config["buildvariants"][0]
        self.assertEqual(first_generated_build_variant["display_tasks"][0]["name"], "burn_in_tests")
        self.assertEqual(
            first_generated_build_variant["display_tasks"][0]["execution_tasks"][0],
            "burn_in:enterprise-rhel-62-64-bit-inmem-required_aggregation_mongos_passthrough_0")
