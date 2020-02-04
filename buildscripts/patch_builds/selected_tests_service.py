#!/usr/bin/env python3
"""Selected Tests service."""

from typing import Any, Dict, Set

import requests
import yaml

# pylint: disable=wrong-import-position
from buildscripts.burn_in_tests import is_file_a_test_file


class SelectedTestsService(object):
    """Selected-tests client object."""

    def __init__(self, url: str, project: str, auth_user: str, auth_token: str):
        """
        Create selected-tests client object.

        :param url: Selected-tests service url.
        :param project: Selected-tests service project.
        :param auth_user: Selected-tests service auth user to authenticate request.
        :param auth_token: Selected-tests service auth token to authenticate request.
        """
        self.url = url
        self.project = project
        self.auth_user = auth_user
        self.auth_token = auth_token
        self.headers = {"Content-type": "application/json", "Accept": "application/json"}
        self.cookies = {"auth_user": auth_user, "auth_token": auth_token}

    @classmethod
    def from_file(cls, filename: str):
        """
        Read config from given filename.

        :param filename: Filename to read config.
        :return: Config read from file.
        """
        with open(filename, 'r') as fstream:
            config = yaml.safe_load(fstream)
            if config:
                return cls(config["url"], config["project"], config["auth_user"],
                           config["auth_token"])

        return None

    def get_test_mappings(self, threshold: float, changed_files: Set[str]) -> Dict[str, Any]:
        """
        Request related test files from selected-tests service.

        :param threshold: Threshold for test file correlation.
        :param changed_files: Set of changed_files.
        :return: Related test files returned by selected-tests service.
        """
        payload = {"threshold": threshold, "changed_files": ",".join(changed_files)}
        response = requests.get(
            self.url + f"/projects/{self.project}/test-mappings",
            params=payload,
            headers=self.headers,
            cookies=self.cookies,
        )
        response.raise_for_status()

        return response.json()["test_mappings"]

    def get_task_mappings(self, threshold: float, changed_files: Set[str]) -> Dict[str, Any]:
        """
        Request related tasks from selected-tests service.

        :param threshold: Threshold for test file correlation.
        :param changed_files: Set of changed_files.
        :return: Related tasks returned by selected-tests service.
        """
        payload = {"threshold": threshold, "changed_files": ",".join(changed_files)}
        response = requests.get(
            self.url + f"/projects/{self.project}/task-mappings",
            params=payload,
            headers=self.headers,
            cookies=self.cookies,
        )
        response.raise_for_status()

        return response.json()["task_mappings"]
