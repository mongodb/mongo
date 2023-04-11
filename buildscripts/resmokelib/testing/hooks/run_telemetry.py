"""
Test hook for verifying $telemetry collects expected metrics and can redact query shapes.

This runs in the background as other tests are ongoing.
"""
import os.path

from buildscripts.resmokelib.testing.hooks.background_job import BackgroundRepeatedJsHook


# Running in the background will better stress concurrency control with other operations.
class RunTelemetry(BackgroundRepeatedJsHook):
    """Periodically runs $telemetry."""

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize RunTelemetry."""

        description = "Read telemetry data concurrently with ongoing tests"
        js_filename = os.path.join("jstests", "hooks", "run_telemetry.js")
        super().__init__(hook_logger, fixture, js_filename, description, "Telemetry",
                         shell_options=shell_options)
