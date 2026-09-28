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


def get_extra_retry_evergreen_api(evergreen_config: str) -> EvergreenApi:
    """
    Build an Evergreen API client that retries harder on transient API errors.

    ``get_api`` mounts its ``HTTPAdapter`` at construction, so reassigning ``_http_retry`` alone
    has no effect -- the adapter has to be re-mounted for the new policy to apply.

    :param evergreen_config: Location of the Evergreen configuration file.
    :return: Evergreen API client whose session retries ``EXTRA_HTTP_RETRY_ATTEMPTS`` more times.
    """
    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config, log_on_error=True)

    retry = Retry(
        total=DEFAULT_HTTP_RETRY_ATTEMPTS + EXTRA_HTTP_RETRY_ATTEMPTS,
        backoff_factor=DEFAULT_HTTP_RETRY_BACKOFF_FACTOR,
        status_forcelist=DEFAULT_HTTP_RETRY_CODES,
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
