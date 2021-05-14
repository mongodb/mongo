"""Unit tests for the selected_tests service."""
import os
import unittest

from tempfile import TemporaryDirectory
import requests
from mock import MagicMock, patch

# pylint: disable=wrong-import-position
import buildscripts.patch_builds.selected_tests.selected_tests_client as under_test

# pylint: disable=missing-docstring


def build_mock_test_mapping(source_file, test_file):
    return under_test.TestMapping(
        branch="branch", project="project", repo="repo", source_file=source_file,
        source_file_seen_count=5, test_files=[
            under_test.TestFileInstance(name=test_file, test_file_seen_count=3),
        ])


def build_mock_task_mapping(source_file, task):
    return under_test.TaskMapping(
        branch="branch", project="project", repo="repo", source_file=source_file,
        source_file_seen_count=5, tasks=[
            under_test.TaskMapInstance(name=task, variant="variant", flip_count=3),
        ])


class TestSelectedTestsClient(unittest.TestCase):
    def test_from_file_with_valid_file(self):
        with TemporaryDirectory() as tmpdir:
            config_file = os.path.join(tmpdir, "selected_tests_test_config.yml")
            with open(config_file, "w") as fh:
                fh.write("url: url\n")
                fh.write("project: project\n")
                fh.write("auth_user: user\n")
                fh.write("auth_token: token\n")

            selected_tests_service = under_test.SelectedTestsClient.from_file(config_file)

            self.assertEqual(selected_tests_service.url, "url")
            self.assertEqual(selected_tests_service.project, "project")
            self.assertEqual(selected_tests_service.session.cookies["auth_user"], "user")
            self.assertEqual(selected_tests_service.session.cookies["auth_token"], "token")

    def test_from_file_with_invalid_file(self):
        with self.assertRaises(FileNotFoundError):
            under_test.SelectedTestsClient.from_file("")

    @patch("requests.Session")
    def test_files_returned_from_selected_tests_service(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response_object = under_test.TestMappingsResponse(test_mappings=[
            build_mock_test_mapping("src/file1.cpp", "jstests/file-1.js"),
            build_mock_test_mapping("src/file2.cpp", "jstests/file-3.js"),
        ])
        requests_mock.return_value.get.return_value.json.return_value = response_object.dict()

        related_test_files = under_test.SelectedTestsClient("my-url.com", "my-project", "auth_user",
                                                            "auth_token").get_test_mappings(
                                                                0.1, changed_files)

        self.assertEqual(related_test_files, response_object)

    @patch("requests.Session")
    def test_selected_tests_service_unavailable(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response = MagicMock(status_code=requests.codes.SERVICE_UNAVAILABLE)
        requests_mock.return_value.get.side_effect = requests.HTTPError(response=response)

        with self.assertRaises(requests.exceptions.HTTPError):
            under_test.SelectedTestsClient("my-url.com", "my-project", "auth_user",
                                           "auth_token").get_test_mappings(0.1, changed_files)

    @patch("requests.Session")
    def test_no_files_returned(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response_object = under_test.TestMappingsResponse(test_mappings=[])
        requests_mock.return_value.get.return_value.json.return_value = response_object.dict()

        related_test_files = under_test.SelectedTestsClient("my-url.com", "my-project", "auth_user",
                                                            "auth_token").get_test_mappings(
                                                                0.1, changed_files)

        self.assertEqual(related_test_files, response_object)
