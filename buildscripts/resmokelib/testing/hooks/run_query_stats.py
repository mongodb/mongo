"""
Test hook for verifying $queryStats collects expected metrics and can redact query shapes.

This runs in the background as other tests are ongoing.
"""

from buildscripts.resmokelib.testing.hooks.interface import Hook
from bson import binary


class RunQueryStats(Hook):
    """Runs $queryStats after every test, and clears the query stats store before every test."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        description = "Read query stats data after each test."
        super().__init__(hook_logger, fixture, description)
        self.client = self.fixture.mongo_client()
        self.hmac_key = binary.Binary(("0" * 32).encode('utf-8'), 8)

    def verify_query_stats(self, querystats_spec):
        """Verify a $queryStats call has all the right properties."""
        with self.client.admin.aggregate([{"$queryStats": querystats_spec}]) as cursor:
            for operation in cursor:
                assert "key" in operation
                assert "metrics" in operation
                assert "asOf" in operation

    def after_test(self, test, test_report):
        self.verify_query_stats({})
        self.verify_query_stats(
            {"transformIdentifiers": {"algorithm": "hmac-sha-256", "hmacKey": self.hmac_key}})

    def before_test(self, test, test_report):
        self.client.admin.command("setParameter", 1, internalQueryStatsCacheSize="0%")
        self.client.admin.command("setParameter", 1, internalQueryStatsCacheSize="1%")
