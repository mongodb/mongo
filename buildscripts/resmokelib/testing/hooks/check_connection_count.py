"""Test hook that verifies mongod/mongos connection churn stays within expected limits."""

import pymongo

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture
from buildscripts.resmokelib.testing.fixtures.interface import build_client
from buildscripts.resmokelib.testing.fixtures.shardedcluster import _MongoSFixture
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.utils import jscomment


class CheckConnectionCount(interface.Hook):
    """Check that mongod/mongos connection churn stays within expected limits.

    This hook reads connPoolStats.totalCreated before and after each test
    and fails if a node opened more than max_connections_per_test connections during
    the test.
    """

    IS_BACKGROUND = False

    ALLOW_EXCEED_TAG = "can_exceed_connection_limit"

    def __init__(
        self,
        hook_logger,
        fixture,
        max_connections_per_test=1000,
        skip_tags=None,
        shell_options=None,
    ):
        """Initialize CheckConnectionCount.

        Args:
            hook_logger: Logger instance for this hook.
            fixture: Target resmoke fixture.
            max_connections_per_test: Maximum allowed increase in totalCreated per node
                during a single test.
            skip_tags: Optional list of test tags for which this hook should not run.
            shell_options: Optional shell options used when building pymongo clients.
        """
        description = "Check connection count"
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self.max_connections_per_test = max_connections_per_test
        self._skip_tags = set(skip_tags) if skip_tags else set()
        self.shell_options = shell_options
        self._baseline_pool_stats = {}

    def _should_skip_test(self, test):
        """Return True if the hook should not run for this test."""
        test_tags = set(jscomment.get_tags(test.test_name))
        if self.ALLOW_EXCEED_TAG in test_tags:
            self.logger.info(
                f"Skipping CheckConnectionCount for {test.test_name} due to tag: "
                f"{self.ALLOW_EXCEED_TAG}"
            )
            return True

        matched = self._skip_tags & test_tags
        if matched:
            self.logger.info(
                f"Skipping CheckConnectionCount for {test.test_name} due to tags: {matched}"
            )
            return True

        return False

    def _iter_nodes(self):
        """Yield all mongod and mongos nodes in the fixture."""
        for cluster in self.fixture.get_independent_clusters():
            for node in cluster._all_mongo_d_s_t():
                if isinstance(node, (MongoDFixture, ExternalFixture, _MongoSFixture)):
                    yield node

    def _get_pool_stats(self, node, phase):
        """Read connPoolStats for a node."""
        node_url = node.get_driver_connection_url()
        try:
            auth_options = (
                self.shell_options
                if self.shell_options and "authenticationMechanism" in self.shell_options
                else None
            )
            node_client = build_client(node, auth_options, pymongo.ReadPreference.PRIMARY_PREFERRED)
            try:
                pool_stats = node_client.admin.command("connPoolStats")
                return pool_stats
            finally:
                node_client.close()
        except Exception as err:
            message = f"Failed to read {phase} connPoolStats for node {node_url}: {err}"
            self.logger.error(message)
            raise errors.TestFailure(message) from err

    def _check_pool_stats(self, test_name, node, violations):
        node_url = node.get_driver_connection_url()
        pool_stats = self._get_pool_stats(node, "post-test")
        baseline_stats = self._baseline_pool_stats.get(node)
        if baseline_stats is None:
            message = (
                f"Missing baseline connPoolStats for node {node_url} while running '{test_name}'"
            )
            self.logger.error(message)
            raise errors.TestFailure(message)

        try:
            baseline = baseline_stats["totalCreated"]
            total_created = pool_stats["totalCreated"]
            connections_opened = total_created - baseline

            if connections_opened > self.max_connections_per_test:
                violation_msg = (
                    f"Test '{test_name}' node {node_url} "
                    f"connections created {connections_opened} "
                    f"exceeds limit {self.max_connections_per_test}. "
                    f"Baseline: {baseline}, Current: {total_created}"
                    "\nPer-pool stats:"
                )

                for pool_name in pool_stats["pools"]:
                    baseline_per_pool = baseline_stats["pools"].get(pool_name, {"poolCreated": 0})
                    baseline_created = baseline_per_pool["poolCreated"]
                    per_pool_stats = pool_stats["pools"][pool_name]
                    pool_created = per_pool_stats["poolCreated"]
                    pool_opened = pool_created - baseline_created

                    violation_msg += (
                        f"\n\tPool: {pool_name} Created: {pool_opened}, "
                        f"Baseline: {baseline_created}, Current: {pool_created}"
                    )

                violations.append(violation_msg)
                self.logger.error(violation_msg)
            else:
                self.logger.info(
                    f"Test '{test_name}' node {node_url} "
                    f"connections created {connections_opened} "
                    f"is below limit {self.max_connections_per_test}"
                )

        except Exception as err:
            message = f"Failed to check connPoolStats object for {node_url}: {err}"
            self.logger.error(message)
            raise errors.TestFailure(message) from err

    def before_test(self, test, test_report):
        """Capture baseline connection counts before test execution."""
        if self._should_skip_test(test):
            return

        for node in self._iter_nodes():
            self._baseline_pool_stats[node] = self._get_pool_stats(node, "baseline")

    def after_test(self, test, test_report):
        """Check connection counts after each test."""
        if self._should_skip_test(test):
            return

        hook_test_case = CheckConnectionCountTestCase.create_after_test(test.logger, test, self)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class CheckConnectionCountTestCase(interface.DynamicTestCase):
    """Dynamic test case for CheckConnectionCount."""

    def run_test(self):
        """Verify that connection churn during the test stayed within limits."""
        violations = []

        for node in self._hook._iter_nodes():
            self._hook._check_pool_stats(self._base_test_name, node, violations)

        if violations:
            raise errors.TestFailure(
                "Connection count exceeded expected limits:\n" + "\n".join(violations)
            )
