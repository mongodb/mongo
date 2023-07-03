"""Test hook for running the $operationMetrics stage in the background.

This hook runs continuously, but the run_aggregate_metrics_background.js file it runs will
internally sleep for 1 second between runs.
"""

import pymongo
import random

from buildscripts.resmokelib.testing.hooks.bghook import BGHook


class AggregateResourceConsumptionMetricsInBackground(BGHook):
    """A hook to run $operationMetrics stage in the background."""

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize AggregateResourceConsumptionMetricsInBackground."""

        description = "Run background $operationMetrics on all mongods while a test is running"
        super().__init__(hook_logger, fixture, description, tests_per_cycle=None,
                         loop_delay_ms=1000)

    def run_action(self):
        """Collects $operationMetrics on all non-arbiter nodes in the fixture."""
        for node_info in self.fixture.get_node_info():
            conn = pymongo.MongoClient(port=node_info.port)
            # Filter out arbiters.
            if "arbiterOnly" in conn.admin.command({"isMaster": 1}):
                self.logger.info(
                    "Skipping background aggregation against test node: %s because it is an " +
                    "arbiter and has no data.", node_info.full_name)
                return

            # Clear the metrics about 10% of the time.
            clear_metrics = random.random() < 0.1
            self.logger.info("Running $operationMetrics with {clearMetrics: %s} on host: %s",
                             clear_metrics, node_info.full_name)
            with conn.admin.aggregate(
                [{"$operationMetrics": {"clearMetrics": clear_metrics}}]) as cursor:
                for doc in cursor:
                    try:
                        self.verify_metrics(doc)
                    except:
                        self.logger.info(
                            "caught exception while verifying that all expected fields are in the" +
                            " metrics output: ", doc)
                        raise

    def verify_metrics(self, doc):
        """Checks whether the output from $operatiomMetrics has the schema we expect."""

        top_level_fields = [
            "docBytesWritten", "docUnitsWritten", "idxEntryBytesWritten", "idxEntryUnitsWritten",
            "totalUnitsWritten", "cpuNanos", "db", "primaryMetrics", "secondaryMetrics"
        ]
        read_fields = [
            "docBytesRead", "docUnitsRead", "idxEntryBytesRead", "idxEntryUnitsRead", "keysSorted",
            "docUnitsReturned"
        ]

        for key in top_level_fields:
            assert key in doc, ("The metrics output is missing the property: " + key)

        primary_metrics = doc["primaryMetrics"]
        for key in read_fields:
            assert key in primary_metrics, (
                "The metrics output is missing the property: primaryMetrics." + key)

        secondary_metrics = doc["secondaryMetrics"]
        for key in read_fields:
            assert key in secondary_metrics, (
                "The metrics output is missing the property: secondaryMetrics." + key)
