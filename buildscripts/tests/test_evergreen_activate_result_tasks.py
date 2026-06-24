"""Unit tests for buildscripts/evergreen_activate_result_tasks.py."""

import unittest
from unittest.mock import patch

from buildscripts import evergreen_activate_result_tasks as under_test
from evergreen.api import DEFAULT_HTTP_RETRY_ATTEMPTS, RetryingEvergreenApi

API_SERVER = "https://evergreen.example.com"


class TestGetEvergreenApi(unittest.TestCase):
    def test_retry_policy_is_actually_mounted_on_session(self):
        # A real RetryingEvergreenApi builds a "sticky" session at construction time with an
        # HTTPAdapter already mounted using the default Retry policy.
        real_api = RetryingEvergreenApi(api_server=API_SERVER)

        # Sanity check: out of the box the session only retries the default number of times,
        # and (urllib3 default) does not retry POST -- the verb task activation actually uses.
        default_adapter = real_api.session.get_adapter(API_SERVER)
        self.assertEqual(default_adapter.max_retries.total, DEFAULT_HTTP_RETRY_ATTEMPTS)
        self.assertNotIn("POST", default_adapter.max_retries.allowed_methods)

        with patch.object(under_test.RetryingEvergreenApi, "get_api", return_value=real_api):
            evg_api = under_test.get_evergreen_api("ignored.yml")

        expected_total = DEFAULT_HTTP_RETRY_ATTEMPTS + under_test.EXTRA_HTTP_RETRY_ATTEMPTS

        # The adapter that actually serves requests must use the more aggressive retry policy.
        # This is the regression guard: reassigning evg_api._http_retry alone does NOT do this.
        adapter = evg_api.session.get_adapter(API_SERVER)
        self.assertEqual(adapter.max_retries.total, expected_total)
        # allowed_methods=False means "retry every verb", so the POST used to activate tasks
        # (the actual 503 failure mode) is retried. This is the point of the fix.
        self.assertIs(adapter.max_retries.allowed_methods, False)

    def test_get_api_called_with_config_file(self):
        real_api = RetryingEvergreenApi(api_server=API_SERVER)
        with patch.object(
            under_test.RetryingEvergreenApi, "get_api", return_value=real_api
        ) as mock_get_api:
            under_test.get_evergreen_api("/path/to/.evergreen.yml")

        mock_get_api.assert_called_once_with(
            config_file="/path/to/.evergreen.yml", log_on_error=True
        )


if __name__ == "__main__":
    unittest.main()
