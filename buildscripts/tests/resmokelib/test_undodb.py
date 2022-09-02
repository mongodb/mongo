"""Fetch subcommand unittest."""
import os
import unittest
from mock import MagicMock, Mock, patch, call
import evergreen
import buildscripts.resmokelib.undodb.fetch as fetch


class TestFetch(unittest.TestCase):
    """Unit tests for the Fetch subcommand."""

    @patch.object(evergreen.RetryingEvergreenApi, "get_api")
    @patch("buildscripts.resmokelib.undodb.fetch.urlopen")
    @patch("buildscripts.resmokelib.undodb.fetch.copyfileobj")
    @patch("tarfile.open")
    def test_fetch(self, tarfile_open_mock, copyfileobj_mock, urlopen_mock, get_api_mock):
        api_mock = MagicMock()
        get_api_mock.return_value = api_mock
        api_mock.task_by_id.return_value = evergreen.task.Task({
            "artifacts": [{
                "name": "UndoDB Recordings - Execution 1",
                "url": "fake://somewhere.over/the/rainbow.tgz",
            }]
        }, api_mock)

        subcommand = fetch.Fetch("task_id")
        subcommand.execute()

        # assert calls to urlopen for downloading the archive
        urlopen_mock.assert_called_once()
        copyfileobj_mock.assert_called_once()

        # calls to tarfile to extract the archive
        tarfile_open_mock.assert_called_once()
