import threading

from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture
from buildscripts.resmokelib.testing.fixtures.interface import MultiClusterFixture
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.testing.hooks.bghook import BGHook

auth_keys = ["username", "password"]
# Name of the availability canaries collection in Atlas.
collection = "availability_01994e24-d524-7b1c-a710-a2f978f41301"

# Canary client configuration parameters used in Atlas.
socketTimeoutMS = 60 * 1000
maxPoolSize = 4


class RunCanaryWorkloadInBackground(BGHook):
    """A hook for running the canary workload while a test is running.

    The main background thread is started before each test and stopped after each test. On each iteration (every 1s),
    it starts one thread per mongod that runs the canary workload against that mongod, using a single client connection
    per node.
    """

    IS_BACKGROUND = True

    SKIP_TESTS = [
        # Skip tests that look at various server status statistics affected by the canaries.
        "jstests/aggregation/sources/lookup/lookup_query_stats.js",
        "jstests/noPassthroughWithMongod/capped/getmore_awaitdata_opcounters.js",
        # Skip tests where background writes could interfere with test expectations.
        "jstests/noPassthrough/traffic_recording/traffic_recording.js",
    ]

    def __init__(self, hook_logger, fixture, shell_options={}):
        """Initialize RunCanaryWorkloadInBackground."""
        description = "Runs the availability canaries against all mongods on the topology while a test is running"
        BGHook.__init__(
            self, hook_logger, fixture, description, tests_per_cycle=1, loop_delay_ms=1000
        )

        hook_logger.info("Loading canaries.")
        self.fixture = fixture
        self.logger = hook_logger
        self.auth_options = {k: v for k, v in shell_options.items() if k in auth_keys}
        self.canaries = {}
        self.background_threads = []

    def run_action(self):
        """Run one iteration of the canary workload against all mongods in the fixture."""
        if isinstance(self.fixture, MultiClusterFixture):
            # For BulkWriteFixture, run canaries against all clusters.
            for cluster in self.fixture.clusters:
                self.run_canary_on_fixture(cluster)
            return
        else:
            self.run_canary_on_fixture(self.fixture)

    def run_canary_on_fixture(self, fixture):
        """Run one iteration of the canary workload against each mongod node in the given fixture."""
        for node in fixture._all_mongo_d_s_t():
            if not isinstance(node, MongoDFixture) and not isinstance(node, ExternalFixture):
                continue

            canary = self.canaries.get(node)  # Ensure a canary is created for this node.
            if canary is None:
                canary = Canary(node, self.auth_options)
                self.canaries[node] = canary

            thread = BackgroundCanary(canary, self.logger)
            thread.start()
            self.background_threads.append(thread)

    def join_background_threads(self):
        """Join all background canary threads started in this iteration."""
        for thread in self.background_threads:
            thread.join()
        self.background_threads = []

    def before_suite(self, test_report):
        super().before_suite(test_report)

        self.logger.info("Stopping the background thread, it'll be started on a per-test basis")
        self.join_background_threads()
        self._background_job.kill()
        self._background_job.join()

        if self._background_job.err is not None:
            self.logger.error("Encountered an error inside the hook: %s.", self._background_job.err)
            raise self._background_job.err

    def before_test(self, test, test_report):
        if test.test_name in self.SKIP_TESTS:
            self.logger.info(
                "Availability canaries explicitly disabled for test `%s`", test.test_name
            )
            return

        self.logger.info(
            "Resuming background availability canaries thread for test `%s`", test.test_name
        )
        super().before_test(test, test_report)
        self.canaries = {}

    def after_test(self, test, test_report):
        if test.test_name in self.SKIP_TESTS:
            return

        super().after_test(test, test_report)
        self.join_background_threads()

        for node, canary in self.canaries.items():
            successes, failures = canary.get_results()
            self.logger.info(
                f"Canary results for node {str(node.port)}: "
                f"successes={successes}, failures={failures}"
            )


class BackgroundCanary(threading.Thread):
    """A thread for running one iteration of the canary workload against a given node."""

    def __init__(self, canary, logger):
        """Initialize BackgroundCanary."""
        threading.Thread.__init__(
            self, name="BackgroundCanaryThread-" + str(canary.node.port), daemon=True
        )
        self.client = canary.client
        self.counter = canary.counter
        self.logger = logger

    def run(self):
        """Run the canary workload.

        This workload replicates the behavior of the "replica set workload" on individual replica sets."""

        try:
            if self.client.is_primary:
                response = self.client.get_database("config").command(
                    {
                        "update": collection,
                        "updates": [
                            {
                                "q": {"_id": 1},
                                "u": {
                                    "$currentDate": {"lastModified": True},
                                    "$inc": {"sequenceNumber": 1},
                                },
                                "upsert": True,
                                "multi": False,
                            }
                        ],
                        "maxTimeMS": 1000,
                    }
                )
                with self.counter.lock:
                    if response.get("ok") != 1:
                        self.logger.info("Failed write canary against primary node.")
                        self.counter.failures += 1
                    else:
                        self.counter.successes += 1

            # For primaries and secondaries
            response = self.client.get_database("config").command(
                {
                    "find": collection,
                    "filter": {"_id": 1},
                    "projection": {
                        "sequenceNumber": 1,
                        "lastModified": 1,
                        "currentDate": "$$NOW",
                        "lagMillis": {"$subtract": ["$$NOW", "$lastModified"]},
                    },
                    "batchSize": 1,
                    "singleBatch": True,
                    "maxTimeMS": 1000,
                }
            )
            with self.counter.lock:
                if response.get("ok") != 1:
                    self.logger.info("Failed read canary.")
                    self.counter.failures += 1
                else:
                    self.counter.successes += 1
        except Exception:
            self.logger.exception("Exception occurred while running canary workload.")
            with self.counter.lock:
                self.counter.failures += 1


class Canary:
    """A class to run the canary workload against a given node."""

    def __init__(self, node, auth_options={}):
        """Initialize Canary."""
        self.client = node.mongo_client(
            maxPoolSize=maxPoolSize, socketTimeoutMS=socketTimeoutMS, **auth_options
        )
        self.node = node
        self.counter = Counter()

    def get_results(self):
        """Get the current results of the canary workload."""
        with self.counter.lock:
            return self.counter.successes, self.counter.failures


class Counter:
    """A simple counter to track successes and failures."""

    def __init__(self):
        """Initialize the Counter."""
        self.successes = 0
        self.failures = 0
        self.lock = threading.Lock()
