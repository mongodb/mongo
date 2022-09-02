"""Test hook for dropping databases created by the fixture."""

import copy

from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import shardedcluster
from buildscripts.resmokelib.testing.hooks import interface


class CleanupConcurrencyWorkloads(interface.Hook):
    """Drop all databases, except those that have been excluded.

    For concurrency tests that run on different DBs, drop all databases except ones
    in 'exclude_dbs'.
    For tests that run on the same DB, drop all databases except ones in 'exclude_dbs'
    and the DB used by the test/workloads.
    For tests that run on the same collection, drop all collections in all databases
    except for 'exclude_dbs' and the collection used by the test/workloads.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, exclude_dbs=None, same_collection=False,
                 same_db=False):
        """Initialize CleanupConcurrencyWorkloads."""
        description = "CleanupConcurrencyWorkloads drops all databases in the fixture"
        interface.Hook.__init__(self, hook_logger, fixture, description)

        protected_dbs = ["admin", "config", "local", "$external"]
        self.exclude_dbs = list(set().union(protected_dbs, utils.default_if_none(exclude_dbs, [])))
        self.same_collection_name = None
        self.same_db_name = None
        if same_db or same_collection:
            # The db name is defined in jstests/concurrency/fsm_utils/name_utils.js.
            self.same_db_name = "fsmdb0"
        if same_collection:
            # The collection name is defined in jstests/concurrency/fsm_utils/name_utils.js.
            self.same_collection_name = "fsmcoll0"

    def after_test(self, test, test_report):
        """After test cleanup."""
        hook_test_case = CleanupConcurrencyWorkloadsTestCase.create_after_test(
            test.logger, test, self)
        hook_test_case.configure(self.fixture)
        hook_test_case.run_dynamic_test(test_report)


class CleanupConcurrencyWorkloadsTestCase(interface.DynamicTestCase):
    """DropDatabasesTestCase class."""

    def _find_same_db_name(self, dbs):
        """Find full name of same_db_name."""
        for db in dbs:
            if db.endswith(self._hook.same_db_name):
                return db
        return None

    def run_test(self):
        """Execute drop databases hook."""
        same_db_name = None
        client = self._hook.fixture.mongo_client()
        db_names = client.database_names()

        exclude_dbs = copy.copy(self._hook.exclude_dbs)
        if self._hook.same_db_name:
            same_db_name = self._find_same_db_name(db_names)
            if same_db_name:
                exclude_dbs.append(same_db_name)
        self.logger.info("Dropping all databases except for %s", exclude_dbs)

        is_sharded_fixture = isinstance(self._hook.fixture, shardedcluster.ShardedClusterFixture)
        # Stop the balancer.
        if is_sharded_fixture and self._hook.fixture.enable_balancer:
            self._hook.fixture.stop_balancer()

        for db_name in [db for db in db_names if db not in exclude_dbs]:
            self.logger.info("Dropping database %s", db_name)
            try:
                client.drop_database(db_name)
            except:
                self.logger.exception("Encountered an error while dropping database %s.", db_name)
                raise

        if self._hook.same_collection_name and same_db_name:
            self.logger.info("Dropping all collections in db %s except for %s", same_db_name,
                             self._hook.same_collection_name)
            colls = client[same_db_name].collection_names()
            for coll in [coll for coll in colls if coll != self._hook.same_collection_name]:
                self.logger.info("Dropping db %s collection %s", same_db_name, coll)
                try:
                    client[same_db_name].drop_collection(coll)
                except:
                    self.logger.exception("Encountered an error while dropping db % collection %s.",
                                          same_db_name, coll)
                    raise

        # Start the balancer.
        if is_sharded_fixture and self._hook.fixture.enable_balancer:
            self._hook.fixture.start_balancer()
