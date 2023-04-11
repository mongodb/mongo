"""Test hook for running the $operationMetrics stage in the background.

This hook runs continuously, but the run_aggregate_metrics_background.js file it runs will
internally sleep for 1 second between runs.
"""

import os.path

from buildscripts.resmokelib.testing.hooks.background_job import BackgroundRepeatedJsHook


class AggregateResourceConsumptionMetricsInBackground(BackgroundRepeatedJsHook):
    """A hook to run $operationMetrics stage in the background."""

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize AggregateResourceConsumptionMetricsInBackground."""

        description = "Run background $operationMetrics on all mongods while a test is running"
        js_filename = os.path.join("jstests", "hooks", "run_aggregate_metrics_background.js")
        super().__init__(hook_logger, fixture, js_filename, description,
                         "AggregateResourceConsumptionMetricsInBackground",
                         shell_options=shell_options)
