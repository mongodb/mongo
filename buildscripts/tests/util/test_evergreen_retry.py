"""Unit tests for buildscripts/util/evergreen_retry.py."""

import unittest
from unittest.mock import patch

from urllib3.exceptions import MaxRetryError

from buildscripts.util import evergreen_retry as under_test
from evergreen.api import (
    DEFAULT_HTTP_RETRY_ATTEMPTS,
    DEFAULT_HTTP_RETRY_CODES,
    RetryingEvergreenApi,
)

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
            evg_api = under_test.get_extra_retry_evergreen_api("ignored.yml")

        expected_total = DEFAULT_HTTP_RETRY_ATTEMPTS + under_test.EXTRA_HTTP_RETRY_ATTEMPTS

        # The adapter that actually serves requests must use the more aggressive retry policy.
        # This is the regression guard: reassigning evg_api._http_retry alone does NOT do this.
        adapter = evg_api.session.get_adapter(API_SERVER)
        self.assertEqual(adapter.max_retries.total, expected_total)
        # allowed_methods=False means "retry every verb", so the POST used to activate tasks
        # (the actual 503 failure mode) is retried. This is the point of the fix.
        self.assertIs(adapter.max_retries.allowed_methods, False)

    def test_transient_auth_and_not_found_codes_are_retried(self):
        # The Evergreen API intermittently returns 401/404 for valid requests, and
        # evergreen.py's default forcelist does not cover them.
        self.assertNotIn(401, DEFAULT_HTTP_RETRY_CODES)
        self.assertNotIn(404, DEFAULT_HTTP_RETRY_CODES)

        real_api = RetryingEvergreenApi(api_server=API_SERVER)
        with patch.object(under_test.RetryingEvergreenApi, "get_api", return_value=real_api):
            evg_api = under_test.get_extra_retry_evergreen_api("ignored.yml")

        retry = evg_api.session.get_adapter(API_SERVER).max_retries
        self.assertIsInstance(retry, under_test.TransientAuthRetry)
        self.assertIn(401, retry.status_forcelist)
        self.assertIn(404, retry.status_forcelist)
        # The evergreen.py defaults must still be honored.
        self.assertTrue(DEFAULT_HTTP_RETRY_CODES.issubset(retry.status_forcelist))

    def test_get_api_called_with_config_file(self):
        real_api = RetryingEvergreenApi(api_server=API_SERVER)
        with patch.object(
            under_test.RetryingEvergreenApi, "get_api", return_value=real_api
        ) as mock_get_api:
            under_test.get_extra_retry_evergreen_api("/path/to/.evergreen.yml")

        mock_get_api.assert_called_once_with(
            config_file="/path/to/.evergreen.yml", log_on_error=True
        )


class FakeResponse:
    """Minimal stand-in for a urllib3 response, which is all Retry.increment() touches."""

    def __init__(self, status: int) -> None:
        self.status = status

    def get_redirect_location(self) -> bool:
        return False


class TestTransientAuthRetry(unittest.TestCase):
    """Exercise the 401/404 cap by driving real Retry.increment() chains."""

    def build_retry(self) -> under_test.TransientAuthRetry:
        return under_test.TransientAuthRetry(
            total=under_test.DEFAULT_HTTP_RETRY_ATTEMPTS + under_test.EXTRA_HTTP_RETRY_ATTEMPTS,
            status_forcelist=under_test.TRANSIENT_HTTP_RETRY_CODES,
            allowed_methods=False,
        )

    def retry_until_exhausted(self, status: int, limit: int = 50) -> int:
        """Retry against a server that always returns ``status``; return the attempts allowed."""
        retry = self.build_retry()
        attempts = 0
        while retry.is_retry("GET", status) and attempts < limit:
            try:
                retry = retry.increment("GET", "/some/url", response=FakeResponse(status))
            except MaxRetryError:
                # Exhausting `total` raises instead of politely returning False (real requests
                # surfaces this as a RetryError). This increment was refused, so it is not an
                # allowed attempt.
                break
            attempts += 1
        return attempts

    def test_transient_auth_codes_stop_after_their_own_small_cap(self):
        for status in sorted(under_test.TRANSIENT_AUTH_HTTP_RETRY_CODES):
            with self.subTest(status=status):
                self.assertEqual(
                    self.retry_until_exhausted(status),
                    under_test.TRANSIENT_AUTH_HTTP_RETRY_ATTEMPTS,
                )

    def test_default_transient_codes_still_get_the_full_budget(self):
        # A 503 must not be shortened by the 401/404 cap.
        self.assertEqual(
            self.retry_until_exhausted(503),
            under_test.DEFAULT_HTTP_RETRY_ATTEMPTS + under_test.EXTRA_HTTP_RETRY_ATTEMPTS,
        )

    def test_transient_auth_backoff_is_flat_and_immediate(self):
        # The default 0.1 backoff factor sleeps 0s on the first retry, which would burn an
        # attempt with no delay at all. The auth path must wait from the very first retry.
        retry = self.build_retry().increment("GET", "/url", response=FakeResponse(401))
        self.assertEqual(
            retry.get_backoff_time(), float(under_test.TRANSIENT_AUTH_HTTP_RETRY_BACKOFF_SEC)
        )

        retry = retry.increment("GET", "/url", response=FakeResponse(401))
        self.assertEqual(
            retry.get_backoff_time(), float(under_test.TRANSIENT_AUTH_HTTP_RETRY_BACKOFF_SEC)
        )

    def test_total_auth_retry_budget_is_about_six_minutes(self):
        retry = self.build_retry()
        total_sleep = 0.0
        while retry.is_retry("GET", 401):
            retry = retry.increment("GET", "/url", response=FakeResponse(401))
            total_sleep += retry.get_backoff_time()

        self.assertEqual(total_sleep, 360.0)

    def test_default_backoff_preserved_for_non_auth_codes(self):
        retry = self.build_retry().increment("GET", "/url", response=FakeResponse(503))
        # First 503 retry uses urllib3's own formula (0s for a single consecutive error),
        # i.e. we did not accidentally apply the flat auth backoff to everything.
        self.assertEqual(retry.get_backoff_time(), 0.0)

    def test_cap_survives_new_which_drops_non_urllib3_state(self):
        # Retry.new() only threads urllib3's own params. The cap must still hold after the
        # copies that increment() makes -- this is why the count is derived from history.
        retry = self.build_retry()
        for _ in range(under_test.TRANSIENT_AUTH_HTTP_RETRY_ATTEMPTS):
            retry = retry.increment("GET", "/url", response=FakeResponse(401))

        self.assertIsInstance(retry, under_test.TransientAuthRetry)
        self.assertFalse(retry.is_retry("GET", 401))
        # Budget for genuinely transient codes is untouched by the auth retries.
        self.assertTrue(retry.is_retry("GET", 503))

    def test_auth_retries_interleaved_with_server_errors(self):
        # A 503 burst must not consume the 401 budget, and vice versa.
        retry = self.build_retry()
        for _ in range(5):
            retry = retry.increment("GET", "/url", response=FakeResponse(503))
        self.assertTrue(retry.is_retry("GET", 401))

        for _ in range(under_test.TRANSIENT_AUTH_HTTP_RETRY_ATTEMPTS):
            retry = retry.increment("GET", "/url", response=FakeResponse(401))
        self.assertFalse(retry.is_retry("GET", 401))


if __name__ == "__main__":
    unittest.main()
