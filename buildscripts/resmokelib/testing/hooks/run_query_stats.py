"""
Test hook for verifying $telemetry collects expected metrics and can redact query shapes.

This runs in the background as other tests are ongoing.
"""
import os.path

from buildscripts.resmokelib.testing.hooks.bghook import BGHook
from bson import binary
import base64


# Running in the background will better stress concurrency control with other operations.
class RunQueryStats(BGHook):
    """Periodically runs $queryStats."""

    def __init__(self, hook_logger, fixture):
        """Initialize RunQueryStats."""

        description = "Read telemetry data concurrently with ongoing tests"
        super().__init__(hook_logger, fixture, description, None, 1000)
        self.client = self.fixture.mongo_client()
        self.hmac_key = binary.Binary(("0" * 32).encode('utf-8'))

    def run_action(self):
        """Runs telemetry on the fixture, and verifies it has the expected shape."""

        self.logger.info("Running $queryStats")

        def verify_results(querystats_spec):
            with self.client.admin.aggregate([{"$queryStats": querystats_spec}]) as cursor:
                for operation in cursor:
                    assert "key" in operation
                    assert "metrics" in operation
                    assert "asOf" in operation

        verify_results({})
        verify_results({"applyHmacToIdentifiers": True, "hmacKey": self.hmac_key})
