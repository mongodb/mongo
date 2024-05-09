# Copyright (c) 2016 testtools developers. See LICENSE for details.

"""Support for testing code that uses Twisted."""

__all__ = [
    # Matchers
    'succeeded',
    'failed',
    'has_no_result',

    # Running tests
    'AsynchronousDeferredRunTest',
    'AsynchronousDeferredRunTestForBrokenTwisted',
    'SynchronousDeferredRunTest',
    'CaptureTwistedLogs',
    'assert_fails_with',
    'flush_logged_errors',
]

from ._matchers import (
    succeeded,
    failed,
    has_no_result,
)

from ._runtest import (
    AsynchronousDeferredRunTest,
    AsynchronousDeferredRunTestForBrokenTwisted,
    SynchronousDeferredRunTest,
    CaptureTwistedLogs,
    assert_fails_with,
    flush_logged_errors,
)
