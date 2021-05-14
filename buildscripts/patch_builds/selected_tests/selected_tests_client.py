#!/usr/bin/env python3
"""Client for accessing selected test app."""

from typing import Set, List, Dict, Any
from urllib.parse import urlparse

import requests
import yaml
from pydantic import BaseModel


class TestFileInstance(BaseModel):
    """
    Frequency of how often a test file was seen.

    name: Name of test file.
    test_file_seen_count: Occurrences of test file.
    """

    name: str
    test_file_seen_count: int


class TestMapping(BaseModel):
    """
    How tests map to the specified source files.

    branch: Git branch being analyzed.
    project: Evergreen project being analyzed.
    repo: Git repo being analyzed.
    source_file: Source file of mappings.
    source_file_seen_count: Number of occurrences of source file.
    test_files: Test files that have been changed with the source file.
    """

    branch: str
    project: str
    repo: str
    source_file: str
    source_file_seen_count: int
    test_files: List[TestFileInstance]


class TestMappingsResponse(BaseModel):
    """
    Response from the test mappings end point.

    test_mappings: List of source files with correlated test files.
    """

    test_mappings: List[TestMapping]


class TaskMapInstance(BaseModel):
    """
    Frequency of how often a task is impacted by a source file change.

    name: Name of task that was impacted.
    variant: Name of build variant task was run on.
    flip_count: Number of times the task was impacted by the source file.
    """

    name: str
    variant: str
    flip_count: int


class TaskMapping(BaseModel):
    """
    How tasks map to the specified source files.

    branch: Git branch being analyzed.
    project: Evergreen project being analyzed.
    repo: Git repo being analyzed.
    source_file: Source file of mappings.
    source_file_seen_count: Number of occurrences of source file.
    tasks: Tasks that have been impacted by the source file.
    """

    branch: str
    project: str
    repo: str
    source_file: str
    source_file_seen_count: int
    tasks: List[TaskMapInstance]


class TaskMappingsResponse(BaseModel):
    """
    Response from the task mappings end point.

    task_mappings: List of source files with correlated tasks.
    """

    task_mappings: List[TaskMapping]


class SelectedTestsClient(object):
    """Selected-tests client object."""

    def __init__(self, url: str, project: str, auth_user: str, auth_token: str) -> None:
        """
        Create selected-tests client object.

        :param url: Selected-tests service url.
        :param project: Selected-tests service project.
        :param auth_user: Selected-tests service auth user to authenticate request.
        :param auth_token: Selected-tests service auth token to authenticate request.
        """
        self.url = url
        self.project = project
        self.session = requests.Session()
        adapter = requests.adapters.HTTPAdapter()
        self.session.mount(f"{urlparse(self.url).scheme}://", adapter)
        self.session.cookies.update({"auth_user": auth_user, "auth_token": auth_token})
        self.session.headers.update(
            {"Content-type": "application/json", "Accept": "application/json"})

    @classmethod
    def from_file(cls, filename: str) -> "SelectedTestsClient":
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

        raise ValueError(f"Could not determine selected tests configuration from {filename}")

    def _call_api(self, endpoint: str, payload: Dict[str, Any]) -> Dict[str, Any]:
        """
        Make a call to the selected tests service and return the response.

        :param endpoint: Endpoint to call.
        :param payload: Payload to call with.
        :return: Response from service.
        """
        url = f"{self.url}{endpoint}"
        response = self.session.get(url, params=payload)
        response.raise_for_status()

        return response.json()

    def get_test_mappings(self, threshold: float, changed_files: Set[str]) -> TestMappingsResponse:
        """
        Request related test files from selected-tests service.

        :param threshold: Threshold for test file correlation.
        :param changed_files: Set of changed_files.
        :return: Related test files returned by selected-tests service.
        """
        payload = {"threshold": threshold, "changed_files": ",".join(changed_files)}
        response = self._call_api(f"/projects/{self.project}/test-mappings", payload)
        return TestMappingsResponse(**response)

    def get_task_mappings(self, threshold: float, changed_files: Set[str]) -> TaskMappingsResponse:
        """
        Request related tasks from selected-tests service.

        :param threshold: Threshold for test file correlation.
        :param changed_files: Set of changed_files.
        :return: Related tasks returned by selected-tests service.
        """
        payload = {"threshold": threshold, "changed_files": ",".join(changed_files)}
        response = self._call_api(f"/projects/{self.project}/task-mappings", payload)
        return TaskMappingsResponse(**response)
