"""Test hook that verifies no commands directly targeted system.buckets collections."""

import pymongo

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture
from buildscripts.resmokelib.testing.fixtures.interface import build_client
from buildscripts.resmokelib.testing.fixtures.shardedcluster import _MongoSFixture
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.testing.hooks import interface


class CheckSystemBucketsMetrics(interface.Hook):
    """Check that no commands directly targeted system.buckets collections.

    This hook reads the numCommandsTargetingSystemBuckets metric from serverStatus
    after each test and fails if the counter has increased.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckSystemBucketsMetrics."""
        description = "Check system.buckets metrics"
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self.shell_options = shell_options
        self._baseline_metrics = {}

    def before_test(self, test, test_report):
        """Capture baseline metrics before test execution."""
        for cluster in self.fixture.get_independent_clusters():
            for node in cluster._all_mongo_d_s_t():
                if not isinstance(node, (MongoDFixture, ExternalFixture, _MongoSFixture)):
                    continue
                metric_value = self._read_metric_or_fail(node, "baseline")
                self._baseline_metrics[node] = metric_value

    def _read_metric_or_fail(self, node, phase):
        """Read the numCommandsTargetingSystemBuckets metric for a node."""
        node_url = node.get_driver_connection_url()
        try:
            auth_options = (
                self.shell_options
                if self.shell_options and "authenticationMechanism" in self.shell_options
                else None
            )
            node_client = build_client(node, auth_options, pymongo.ReadPreference.PRIMARY_PREFERRED)
            try:
                serverStatus = node_client.admin.command("serverStatus")
                return serverStatus["metrics"]["numCommandsTargetingSystemBuckets"]
            finally:
                node_client.close()
        except Exception as err:
            message = f"Failed to read {phase} numCommandsTargetingSystemBuckets metric for node {node_url}: {err}"
            self.logger.error(message)
            raise errors.TestFailure(message) from err

    def after_test(self, test, test_report):
        """Check metrics after each test."""
        hook_test_case = CheckSystemBucketsMetricsTestCase.create_after_test(
            test.logger, test, self
        )
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class CheckSystemBucketsMetricsTestCase(interface.DynamicTestCase):
    """CheckSystemBucketsMetricsTestCase class."""

    def run_test(self):
        """Execute test hook to verify no system.buckets commands were executed."""
        violations = []

        for cluster in self._hook.fixture.get_independent_clusters():
            for node in cluster._all_mongo_d_s_t():
                if not isinstance(node, (MongoDFixture, ExternalFixture, _MongoSFixture)):
                    continue
                node_url = node.get_driver_connection_url()
                current_count = self._hook._read_metric_or_fail(node, "post-test")
                baseline = self._hook._baseline_metrics.get(node, 0)

                if current_count > baseline:
                    violation_msg = (
                        f"Test '{self._base_test_name}' directly targeted system.buckets collection(s). "
                        f"Node: {node_url}, Baseline: {baseline}, Current: {current_count}, "
                        f"Difference: {current_count - baseline}. "
                    )
                    violations.append(violation_msg)
                    self.logger.error(violation_msg)

                # Update baseline to ensure test isolation
                self._hook._baseline_metrics[node] = current_count

        if violations:
            raise errors.TestFailure(
                "Test directly targeted system.buckets collections:\n" + "\n".join(violations)
            )
