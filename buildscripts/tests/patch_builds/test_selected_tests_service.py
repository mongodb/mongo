"""Unit tests for the selected_tests service."""
import os
import unittest

from tempfile import TemporaryDirectory
import requests
from mock import MagicMock, patch

# pylint: disable=wrong-import-position
import buildscripts.patch_builds.selected_tests_service as under_test

# pylint: disable=missing-docstring

NS = "buildscripts.patch_builds.selected_tests_service"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


class TestSelectedTestsService(unittest.TestCase):
    def test_from_file_with_valid_file(self):
        with TemporaryDirectory() as tmpdir:
            config_file = os.path.join(tmpdir, "selected_tests_test_config.yml")
            with open(config_file, "w") as fh:
                fh.write("url: url\n")
                fh.write("project: project\n")
                fh.write("auth_user: user\n")
                fh.write("auth_token: token\n")

            selected_tests_service = under_test.SelectedTestsService.from_file(config_file)

            self.assertEqual(selected_tests_service.url, "url")
            self.assertEqual(selected_tests_service.project, "project")
            self.assertEqual(selected_tests_service.auth_user, "user")
            self.assertEqual(selected_tests_service.auth_token, "token")

    def test_from_file_with_invalid_file(self):
        with self.assertRaises(FileNotFoundError):
            under_test.SelectedTestsService.from_file("")

    @patch(ns("requests"))
    def test_files_returned_from_selected_tests_service(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response_object = {
            "test_mappings": [
                {
                    "source_file": "src/file1.cpp",
                    "test_files": [{"name": "jstests/file-1.js"}],
                },
                {
                    "source_file": "src/file2.cpp",
                    "test_files": [{"name": "jstests/file-3.js"}],
                },
            ]
        }
        requests_mock.get.return_value.json.return_value = response_object

        related_test_files = under_test.SelectedTestsService(
            "my-url.com", "my-project", "auth_user", "auth_token").get_test_mappings(
                0.1, changed_files)

        requests_mock.get.assert_called_with(
            "my-url.com/projects/my-project/test-mappings",
            params={"threshold": 0.1, "changed_files": ",".join(changed_files)},
            headers={
                "Content-type": "application/json",
                "Accept": "application/json",
            },
            cookies={"auth_user": "auth_user", "auth_token": "auth_token"},
        )
        self.assertEqual(related_test_files, response_object["test_mappings"])

    @patch(ns("requests"))
    def test_selected_tests_service_unavailable(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response = MagicMock(status_code=requests.codes.SERVICE_UNAVAILABLE)
        requests_mock.get.side_effect = requests.HTTPError(response=response)

        with self.assertRaises(requests.exceptions.HTTPError):
            under_test.SelectedTestsService("my-url.com", "my-project", "auth_user",
                                            "auth_token").get_test_mappings(0.1, changed_files)

    @patch(ns("requests"))
    def test_no_files_returned(self, requests_mock):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        response_object = {"test_mappings": []}
        requests_mock.get.return_value.json.return_value = response_object

        related_test_files = under_test.SelectedTestsService(
            "my-url.com", "my-project", "auth_user", "auth_token").get_test_mappings(
                0.1, changed_files)

        self.assertEqual(related_test_files, [])
