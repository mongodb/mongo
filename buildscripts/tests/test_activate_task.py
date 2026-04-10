"""Unit tests for evergreen/activate_task.py."""

import importlib.util
import sys
import types
import unittest
from pathlib import Path
from typing import Annotated
from unittest.mock import MagicMock, patch

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "evergreen" / "activate_task.py"


def load_under_test():
    fake_app = MagicMock()
    fake_typer = types.ModuleType("typer")
    fake_typer.Argument = lambda *args, **kwargs: None
    fake_typer.run = lambda *args, **kwargs: None
    fake_typer.Typer = lambda *args, **kwargs: fake_app

    fake_typing_extensions = types.ModuleType("typing_extensions")
    fake_typing_extensions.Annotated = Annotated

    fake_evergreen_conn = types.ModuleType("evergreen_conn")
    fake_evergreen_conn.get_evergreen_api = MagicMock()

    fake_resmokelib = types.ModuleType("buildscripts.resmokelib")
    fake_resmokelib.__path__ = []
    fake_utils = types.ModuleType("buildscripts.resmokelib.utils")
    fake_utils.evergreen_conn = fake_evergreen_conn
    fake_resmokelib.utils = fake_utils

    fake_buildscripts_util = types.ModuleType("buildscripts.util")
    fake_buildscripts_util.__path__ = []
    fake_read_config = types.ModuleType("buildscripts.util.read_config")
    fake_read_config.read_config_file = MagicMock()
    fake_buildscripts_util.read_config = fake_read_config

    with patch.dict(
        sys.modules,
        {
            "typer": fake_typer,
            "typing_extensions": fake_typing_extensions,
            "buildscripts.resmokelib": fake_resmokelib,
            "buildscripts.resmokelib.utils": fake_utils,
            "buildscripts.util": fake_buildscripts_util,
            "buildscripts.util.read_config": fake_read_config,
        },
    ):
        spec = importlib.util.spec_from_file_location("activate_task_under_test", SCRIPT_PATH)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module


under_test = load_under_test()


def build_mock_task(display_name: str, task_id: str = "task_id", activated: bool = False):
    mock_task = MagicMock()
    mock_task.display_name = display_name
    mock_task.task_id = task_id
    mock_task.activated = activated
    return mock_task


class TestActivateTask(unittest.TestCase):
    @patch.object(under_test.evergreen_conn, "get_evergreen_api")
    @patch.object(under_test, "read_config_file")
    def test_default_skip_for_patch_author_does_not_skip_activation(
        self, mock_read_config_file, mock_get_evergreen_api
    ):
        mock_read_config_file.return_value = {
            "build_id": "build_id",
        }
        mock_variant = MagicMock()
        mock_variant.get_tasks.return_value = [
            build_mock_task("archive_dist_test_debug", task_id="task_id")
        ]
        mock_evg_api = MagicMock()
        mock_evg_api.build_by_id.return_value = mock_variant
        mock_get_evergreen_api.return_value = mock_evg_api

        under_test.main("archive_dist_test_debug")

        mock_evg_api.configure_task.assert_called_once_with("task_id", activated=True)

    @patch.object(under_test.evergreen_conn, "get_evergreen_api")
    @patch.object(under_test, "read_config_file")
    def test_missing_task_raises_runtime_error_with_build_variant(
        self, mock_read_config_file, mock_get_evergreen_api
    ):
        mock_read_config_file.return_value = {
            "build_id": "build_id",
            "build_variant": "linux-debug",
            "is_patch": False,
        }
        mock_variant = MagicMock()
        mock_variant.build_variant = None
        mock_variant.get_tasks.return_value = [build_mock_task("some_other_task")]
        mock_evg_api = MagicMock()
        mock_evg_api.build_by_id.return_value = mock_variant
        mock_get_evergreen_api.return_value = mock_evg_api

        with self.assertRaisesRegex(
            RuntimeError,
            "The archive_dist_test_debug task could not be found in the linux-debug variant",
        ):
            under_test.main("archive_dist_test_debug")

    @patch.object(under_test.evergreen_conn, "get_evergreen_api")
    @patch.object(under_test, "read_config_file")
    def test_missing_task_falls_back_to_build_id_when_variant_name_absent(
        self, mock_read_config_file, mock_get_evergreen_api
    ):
        mock_read_config_file.return_value = {
            "build_id": "build_id",
            "is_patch": False,
        }
        mock_variant = MagicMock()
        mock_variant.build_variant = None
        mock_variant.get_tasks.return_value = [build_mock_task("some_other_task")]
        mock_evg_api = MagicMock()
        mock_evg_api.build_by_id.return_value = mock_variant
        mock_get_evergreen_api.return_value = mock_evg_api

        with self.assertRaisesRegex(
            RuntimeError,
            "The archive_dist_test_debug task could not be found in the build_id variant",
        ):
            under_test.main("archive_dist_test_debug")

    @patch.object(under_test.evergreen_conn, "get_evergreen_api")
    @patch.object(under_test, "read_config_file")
    def test_missing_patch_task_uses_build_variant_from_build(
        self, mock_read_config_file, mock_get_evergreen_api
    ):
        mock_read_config_file.return_value = {
            "build_id": "build_id",
            "version_id": "version_id",
            "is_patch": True,
        }
        mock_variant = MagicMock()
        mock_variant.build_variant = "linux-debug"
        mock_variant.get_tasks.return_value = [build_mock_task("some_other_task")]
        mock_evg_api = MagicMock()
        mock_evg_api.build_by_id.return_value = mock_variant
        mock_get_evergreen_api.return_value = mock_evg_api

        under_test.main("archive_dist_test_debug")

        mock_evg_api.configure_patch.assert_called_once_with(
            "version_id", [{"id": "linux-debug", "tasks": ["archive_dist_test_debug"]}]
        )

    @patch.object(under_test.evergreen_conn, "get_evergreen_api")
    @patch.object(under_test, "read_config_file")
    def test_missing_patch_task_raises_when_variant_name_absent(
        self, mock_read_config_file, mock_get_evergreen_api
    ):
        mock_read_config_file.return_value = {
            "build_id": "build_id",
            "version_id": "version_id",
            "is_patch": True,
        }
        mock_variant = MagicMock()
        mock_variant.build_variant = None
        mock_variant.get_tasks.return_value = [build_mock_task("some_other_task")]
        mock_evg_api = MagicMock()
        mock_evg_api.build_by_id.return_value = mock_variant
        mock_get_evergreen_api.return_value = mock_evg_api

        with self.assertRaisesRegex(
            RuntimeError,
            "Could not determine the build variant for patch activation from build build_id",
        ):
            under_test.main("archive_dist_test_debug")
