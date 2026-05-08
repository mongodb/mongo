import time

from pymongo.errors import ConnectionFailure, ExecutionTimeout, OperationFailure, PyMongoError

# TODO(DRIVERS-1401): Use error labels instead of checking against an allow list of error codes.
retryable_codes = [
    # From the SDAM spec, the "node is shutting down" codes.
    11600,  # InterruptedAtShutdown
    91,  # ShutdownInProgress
    # From the SDAM spec, the "not primary" and "node is recovering" error codes.
    10058,  # LegacyNotPrimary <=3.2 "not primary" error code
    10107,  # NotWritablePrimary
    13435,  # NotPrimaryNoSecondaryOk
    11602,  # InterruptedDueToReplStateChange
    13436,  # NotPrimaryOrSecondary
    189,  # PrimarySteppedDown
    # From the retryable reads/writes spec.
    7,  # HostNotFound
    6,  # HostUnreachable
    89,  # NetworkTimeout
    9001,  # SocketException
    262,  # ExceededTimeLimit
    202,  # NetworkInterfaceExceededTimeLimit
]

# The names for the error codes above.
retryable_code_names = [
    "InterruptedAtShutdown",
    "ShutdownInProgress",
    "LegacyNotPrimary",
    "NotWritablePrimary",
    "NotPrimaryNoSecondaryOk",
    "InterruptedDueToReplStateChange",
    "NotPrimaryOrSecondary",
    "PrimarySteppedDown",
    "HostNotFound",
    "HostUnreachable",
    "NetworkTimeout",
    "SocketException",
    "ExceededTimeLimit",
    "NetworkInterfaceExceededTimeLimit",
]


def is_retryable_error(exc, retryable_error_codes):
    # Guard against non-PyMongoError exceptions: has_error_label() only exists on
    # PyMongoError, so calling it on e.g. AssertionError or ServerFailure would raise
    # AttributeError. Return False immediately for anything that isn't a pymongo error.
    if not isinstance(exc, PyMongoError):
        return False
    if isinstance(exc, ConnectionFailure):
        return True
    if exc.has_error_label("RetryableWriteError"):
        return True
    if isinstance(exc, OperationFailure) and exc.code in retryable_error_codes:
        return True
    return False


def with_naive_retry(func, timeout=100, extra_retryable_error_codes=None):
    """
    Retry execution of a provided function naively for up to `timeout` seconds.

    This method is only suitable for reads or other idempotent operations. It is not suitable for
    retrying non-idempotent operations (most writes).

    :param func: The function to execute
    :param timeout: The maximum amount of time to retry execution
    :param extra_retryable_error_codes: List of additional error codes that should be considered retryable
    """

    retryable_error_codes = set(retryable_codes)
    if extra_retryable_error_codes:
        retryable_error_codes.update(extra_retryable_error_codes)

    last_exc = None
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        try:
            return func()
        except PyMongoError as exc:
            last_exc = exc
            if not is_retryable_error(exc, retryable_error_codes):
                raise
        time.sleep(0.1)

    raise ExecutionTimeout(
        f"Operation exceeded time limit after {timeout} seconds, last error: {last_exc}"
    )


def with_predicate_retry(func, is_transient, timeout=30.0, sleep_secs=1.0, on_retry=None):
    """
    Retry execution of `func` while `is_transient(exc)` is True, up to `timeout` seconds.

    Use for non-pymongo transients (e.g. gRPC subprocess errors, Docker compose unavailability)
    where the caller decides which exceptions are retryable.

    :param func: Zero-arg callable to invoke.
    :param is_transient: Callable taking the raised exception, returning True iff retryable.
    :param timeout: Maximum total wall time to retry, seconds.
    :param sleep_secs: Sleep between attempts, seconds.
    :param on_retry: Optional callback `(attempt: int, exc: Exception) -> None` invoked
                    on each transient failure before sleeping. Use for visibility.
    """
    last_exc = None
    attempt = 0
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        attempt += 1
        try:
            return func()
        except Exception as exc:
            last_exc = exc
            if not is_transient(exc):
                raise
            if on_retry is not None:
                on_retry(attempt, exc)
        time.sleep(sleep_secs)

    raise last_exc
