"""Unit tests for configure_resmoke._apply_evergreen_tss_project_config."""

import unittest

from mock import MagicMock, patch

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import configure_resmoke as under_test


class TestApplyEvergreenTssProjectConfig(unittest.TestCase):
    def setUp(self):
        self._orig_task_id = _config.EVERGREEN_TASK_ID
        self._orig_project_name = _config.EVERGREEN_PROJECT_NAME
        self._orig_patch_build = _config.EVERGREEN_PATCH_BUILD
        self._orig_tss_enabled = _config.ENABLE_EVERGREEN_API_TEST_SELECTION

    def tearDown(self):
        _config.EVERGREEN_TASK_ID = self._orig_task_id
        _config.EVERGREEN_PROJECT_NAME = self._orig_project_name
        _config.EVERGREEN_PATCH_BUILD = self._orig_patch_build
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = self._orig_tss_enabled

    def _set_evergreen_context(self, task_id="task123", project_name="mongodb-mongo-master"):
        _config.EVERGREEN_TASK_ID = task_id
        _config.EVERGREEN_PROJECT_NAME = project_name
        _config.EVERGREEN_PATCH_BUILD = True

    def _make_project(self, test_selection):
        project = MagicMock()
        project.json = {"test_selection": test_selection} if test_selection is not None else {}
        return project

    # --- early-return cases: not in Evergreen ---

    def test_no_task_id_skips(self):
        _config.EVERGREEN_TASK_ID = None
        _config.EVERGREEN_PROJECT_NAME = "some-project"
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        with patch.object(under_test.evergreen_conn, "get_evergreen_api") as mock_api:
            under_test._apply_evergreen_tss_project_config()

        mock_api.assert_not_called()
        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_no_project_name_skips(self):
        _config.EVERGREEN_TASK_ID = "task123"
        _config.EVERGREEN_PROJECT_NAME = None
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        with patch.object(under_test.evergreen_conn, "get_evergreen_api") as mock_api:
            under_test._apply_evergreen_tss_project_config()

        mock_api.assert_not_called()
        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_non_patch_build_skips(self):
        self._set_evergreen_context()
        _config.EVERGREEN_PATCH_BUILD = False
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        with patch.object(under_test.evergreen_conn, "get_evergreen_api") as mock_api:
            under_test._apply_evergreen_tss_project_config()

        mock_api.assert_not_called()
        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    # --- early-return cases: CLI flag explicitly set ---

    def test_cli_flag_explicitly_enabled_skips_project_config(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = True  # --enableEvergreenApiTestSelection

        with patch.object(under_test.evergreen_conn, "get_evergreen_api") as mock_api:
            under_test._apply_evergreen_tss_project_config()

        mock_api.assert_not_called()
        self.assertTrue(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_cli_flag_explicitly_disabled_skips_project_config(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = False  # --no-enableEvergreenApiTestSelection

        with patch.object(under_test.evergreen_conn, "get_evergreen_api") as mock_api:
            under_test._apply_evergreen_tss_project_config()

        mock_api.assert_not_called()
        self.assertFalse(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    # --- project config applied (flag not passed → None) ---

    def test_project_config_allowed_true_enables_tss(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        mock_evg_api = MagicMock()
        mock_evg_api.project_by_id.return_value = self._make_project({"allowed": True})
        with patch.object(
            under_test.evergreen_conn, "get_evergreen_api", return_value=mock_evg_api
        ):
            under_test._apply_evergreen_tss_project_config()

        self.assertTrue(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)
        mock_evg_api.project_by_id.assert_called_once_with("mongodb-mongo-master")

    def test_project_config_allowed_false_disables_tss(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        mock_evg_api = MagicMock()
        mock_evg_api.project_by_id.return_value = self._make_project({"allowed": False})
        with patch.object(
            under_test.evergreen_conn, "get_evergreen_api", return_value=mock_evg_api
        ):
            under_test._apply_evergreen_tss_project_config()

        self.assertFalse(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_project_config_missing_test_selection_field_no_change(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        mock_evg_api = MagicMock()
        mock_evg_api.project_by_id.return_value = self._make_project(None)
        with patch.object(
            under_test.evergreen_conn, "get_evergreen_api", return_value=mock_evg_api
        ):
            under_test._apply_evergreen_tss_project_config()

        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_project_config_missing_allowed_field_defaults_to_false(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        mock_evg_api = MagicMock()
        mock_evg_api.project_by_id.return_value = self._make_project({})  # no 'allowed' key
        with patch.object(
            under_test.evergreen_conn, "get_evergreen_api", return_value=mock_evg_api
        ):
            under_test._apply_evergreen_tss_project_config()

        self.assertFalse(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    # --- failure handling ---

    def test_api_failure_logs_warning_and_leaves_tss_unchanged(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        with (
            patch.object(
                under_test.evergreen_conn,
                "get_evergreen_api",
                side_effect=RuntimeError("no .evergreen.yml"),
            ),
            patch("buildscripts.resmokelib.configure_resmoke.logging") as mock_logging,
        ):
            under_test._apply_evergreen_tss_project_config()

        mock_logging.getLogger.return_value.warning.assert_called_once()
        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)

    def test_project_by_id_failure_logs_warning_and_leaves_tss_unchanged(self):
        self._set_evergreen_context()
        _config.ENABLE_EVERGREEN_API_TEST_SELECTION = None  # flag not passed

        mock_evg_api = MagicMock()
        mock_evg_api.project_by_id.side_effect = Exception("HTTP 503")
        with (
            patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=mock_evg_api),
            patch("buildscripts.resmokelib.configure_resmoke.logging") as mock_logging,
        ):
            under_test._apply_evergreen_tss_project_config()

        mock_logging.getLogger.return_value.warning.assert_called_once()
        self.assertIsNone(_config.ENABLE_EVERGREEN_API_TEST_SELECTION)


if __name__ == "__main__":
    unittest.main()
