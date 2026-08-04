"""Build Evergreen API clients that ride out transient API errors."""

import requests
from urllib3.util import Retry

from evergreen.api import (
    DEFAULT_HTTP_RETRY_ATTEMPTS,
    DEFAULT_HTTP_RETRY_BACKOFF_FACTOR,
    DEFAULT_HTTP_RETRY_CODES,
    EvergreenApi,
    RetryingEvergreenApi,
)

# Extra HTTP retry attempts (on top of the evergreen.py default) used when activating tasks.
# Activation talks to the live Evergreen API from inside a task, so we want to ride out
# transient API degradation (e.g. 503s) rather than fail the whole task.
EXTRA_HTTP_RETRY_ATTEMPTS = 10

# evergreen.py's DEFAULT_HTTP_RETRY_CODES covers 408/409/425/429 and 5xx, but the Evergreen
# API also intermittently returns 401 and 404 for requests that are perfectly valid -- we have
# seen a 401 on the *second* page of a paginated tasks_by_build call whose first page succeeded
# with the same session and credentials.
#
# Unlike the default codes, 401 and 404 are normally *terminal*: a genuinely bad credential or a
# genuinely missing resource will never succeed no matter how long we retry. So they get their own
# small, fixed budget instead of the full attempt count.
TRANSIENT_AUTH_HTTP_RETRY_CODES = frozenset({401, 404})
TRANSIENT_AUTH_HTTP_RETRY_ATTEMPTS = 3
TRANSIENT_AUTH_HTTP_RETRY_BACKOFF_SEC = 120

TRANSIENT_HTTP_RETRY_CODES = frozenset(DEFAULT_HTTP_RETRY_CODES | TRANSIENT_AUTH_HTTP_RETRY_CODES)


class TransientAuthRetry(Retry):
    """
    A ``Retry`` that gives 401/404 a smaller and slower budget than the other transient codes.

    There is no per-status-code retry limit to configure, so the cap means overriding the retry
    decision itself. The count comes from ``self.history`` because ``Retry.new()`` would drop an
    attribute stored on the instance.
    """

    def _transient_auth_retries_taken(self) -> int:
        """Count how many retries so far were caused by a transient-auth status code."""
        return sum(1 for entry in self.history if entry.status in TRANSIENT_AUTH_HTTP_RETRY_CODES)

    def is_retry(self, method: str, status_code: int, has_retry_after: bool = False) -> bool:
        """Decide whether to retry, enforcing the separate 401/404 cap."""
        if (
            status_code in TRANSIENT_AUTH_HTTP_RETRY_CODES
            and self._transient_auth_retries_taken() >= TRANSIENT_AUTH_HTTP_RETRY_ATTEMPTS
        ):
            return False
        return super().is_retry(method, status_code, has_retry_after)

    def get_backoff_time(self) -> float:
        """Use the flat transient-auth backoff when the last attempt failed with 401/404."""
        if self.history and self.history[-1].status in TRANSIENT_AUTH_HTTP_RETRY_CODES:
            return float(TRANSIENT_AUTH_HTTP_RETRY_BACKOFF_SEC)
        return super().get_backoff_time()


def get_extra_retry_evergreen_api(evergreen_config: str) -> EvergreenApi:
    """
    Build an Evergreen API client that retries harder on transient API errors.

    ``get_api`` mounts its ``HTTPAdapter`` at construction, so reassigning ``_http_retry`` alone
    has no effect -- the adapter has to be re-mounted for the new policy to apply.

    :param evergreen_config: Location of the Evergreen configuration file.
    :return: Evergreen API client whose session retries ``EXTRA_HTTP_RETRY_ATTEMPTS`` more times.
    """
    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config, log_on_error=True)

    retry = TransientAuthRetry(
        total=DEFAULT_HTTP_RETRY_ATTEMPTS + EXTRA_HTTP_RETRY_ATTEMPTS,
        backoff_factor=DEFAULT_HTTP_RETRY_BACKOFF_FACTOR,
        status_forcelist=TRANSIENT_HTTP_RETRY_CODES,
        # Task activation is a POST, which urllib3's default allowed_methods excludes from
        # retries. Without this, the 503s we are trying to ride out would never be retried.
        allowed_methods=False,
        raise_on_status=False,
        raise_on_redirect=False,
    )
    evg_api._http_retry = retry

    # Re-mount the adapter on the existing sticky session so the new retry policy is applied.
    adapter = requests.adapters.HTTPAdapter(max_retries=retry)
    session = evg_api.session
    for scheme in ("http://", "https://"):
        session.mount(scheme, adapter)

    return evg_api
