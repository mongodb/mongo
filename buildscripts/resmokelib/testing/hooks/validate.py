"""Test hook for verifying the consistency and integrity of collection and index data."""

import concurrent.futures
import logging
import os.path
import time

import pymongo
import pymongo.errors
import pymongo.mongo_client
from pymongo.collection import Collection
from pymongo.database import Database

from buildscripts.resmokelib.testing.fixtures.external import ExternalFixture
from buildscripts.resmokelib.testing.fixtures.interface import build_client
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
from buildscripts.resmokelib.testing.hooks import jsfile


class ValidateCollections(jsfile.PerClusterDataConsistencyHook):
    """Run full validation.

    This will run on all collections in all databases on every stand-alone
    node, primary replica-set node, or primary shard node.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None, use_legacy_validate=False):
        """Initialize ValidateCollections."""
        description = "Full collection validation"
        js_filename = os.path.join("jstests", "hooks", "run_validate_collections.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
        self.use_legacy_validate = use_legacy_validate
        self._catalog_check_js_filename = os.path.join(
            "jstests", "hooks", "run_check_list_catalog_operations_consistency.js"
        )

    def after_test(self, test, test_report):
        # Break the fixture down into its participant clusters if it is a MultiClusterFixture.
        for cluster in self.fixture.get_independent_clusters():
            self.logger.info(
                f"Running ValidateCollections on '{cluster}' with driver URL '{cluster.get_driver_connection_url()}'"
            )
            hook_test_case = ValidateCollectionsTestCase.create_after_test(
                test.logger,
                test,
                self,
                self._js_filename,
                self.use_legacy_validate,
                self._shell_options,
            )
            hook_test_case.configure(cluster)
            hook_test_case.run_dynamic_test(test_report)

            hook_test_case_catalog_check = jsfile.DynamicJSTestCase.create_after_test(
                test.logger, test, self, self._catalog_check_js_filename, self._shell_options
            )
            hook_test_case_catalog_check.configure(self.fixture)
            hook_test_case_catalog_check.run_dynamic_test(test_report)


class ValidateCollectionsTestCase(jsfile.DynamicJSTestCase):
    """ValidateCollectionsTestCase class."""

    def __init__(
        self,
        logger: logging.Logger,
        test_name: str,
        description: str,
        base_test_name: str,
        hook,
        js_filename: str,
        use_legacy_validate: bool,
        shell_options=None,
    ):
        super().__init__(
            logger, test_name, description, base_test_name, hook, js_filename, shell_options
        )
        self.use_legacy_validate = use_legacy_validate
        self.shell_options = shell_options

    def run_test(self):
        """Execute test hook."""
        if self.use_legacy_validate:
            self.logger.info(
                "Running legacy javascript validation because use_legacy_validate is set"
            )
            super().run_test()
            return

        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as executor:
                futures = []
                for node in self.fixture._all_mongo_d_s_t():
                    if not isinstance(node, MongoDFixture) and not isinstance(
                        node, ExternalFixture
                    ):
                        continue

                    if not validate_node(node, self.shell_options, self.logger, executor, futures):
                        raise RuntimeError(
                            f"Internal error while trying to validate node: {node.get_driver_connection_url()}"
                        )

                for future in concurrent.futures.as_completed(futures):
                    exception = future.exception()
                    if exception is not None:
                        executor.shutdown(wait=False, cancel_futures=True)
                        raise RuntimeError(
                            "Collection validation raised an exception."
                        ) from exception
                    result = future.result()
                    if result is not True:
                        raise RuntimeError("Collection validation failed.")
        except:
            self.logger.exception("Uncaught exception while validating collections")
            raise


def validate_node(
    node: MongoDFixture,
    shell_options: dict,
    logger: logging.Logger,
    executor: concurrent.futures.ThreadPoolExecutor,
    futures: list,
) -> bool:
    try:
        auth_options = None
        if shell_options and "authenticationMechanism" in shell_options:
            auth_options = shell_options
        client = build_client(node, auth_options, pymongo.ReadPreference.PRIMARY_PREFERRED)

        # Skip validating collections for arbiters.
        admin_db = client.get_database("admin")
        ret = admin_db.command("isMaster")
        if "arbiterOnly" in ret and ret["arbiterOnly"]:
            logger.info(
                f"Skipping collection validation on arbiter {node.get_driver_connection_url()}"
            )
            return True

        # Skip fast count validation on nodes using FCBIS since FCBIS can result in inaccurate fast
        # counts.
        ret = admin_db.command({"getParameter": 1, "initialSyncMethod": 1})
        skipEnforceFastCountOnValidate = False

        if ret["initialSyncMethod"] == "fileCopyBased":
            logger.info(
                f"Skipping fast count validation against test node: {node.get_driver_connection_url()} because it uses FCBIS and fast count is expected to be incorrect."
            )
            skipEnforceFastCountOnValidate = True

        for db_name in client.list_database_names():
            if not validate_database(
                client,
                db_name,
                shell_options,
                skipEnforceFastCountOnValidate,
                logger,
                executor,
                futures,
            ):
                raise RuntimeError(f"Internal error while validating database: {db_name}")
        return True
    except:
        logger.exception(
            f"Unknown exception while validating node {node.get_driver_connection_url()}"
        )
        return False


def validate_database(
    client: pymongo.mongo_client.MongoClient,
    db_name: str,
    shell_options: dict,
    skipEnforceFastCountOnValidate: bool,
    logger: logging.Logger,
    executor: concurrent.futures.ThreadPoolExecutor,
    futures: list,
):
    try:
        db = client.get_database(db_name)

        shell_options = shell_options or {}
        test_data = shell_options.get("global_vars", {}).get("TestData", {})
        skipEnforceFastCountOnValidate = test_data.get(
            "skipEnforceFastCountOnValidate", skipEnforceFastCountOnValidate
        )
        skipValidationOnInvalidViewDefinitions = test_data.get(
            "skipValidationOnInvalidViewDefinitions", False
        )
        skipValidationOnNamespaceNotFound = test_data.get("skipValidationOnNamespaceNotFound", True)

        validate_opts = {
            "full": True,
            # TODO (SERVER-24266): Always enforce fast counts, once they are always accurate
            "enforceFastCount": not skipEnforceFastCountOnValidate,
        }

        # Don't run validate on view namespaces.
        filter = {"type": "collection"}
        if skipValidationOnInvalidViewDefinitions:
            # If skipValidationOnInvalidViewDefinitions=true, then we avoid resolving the view
            # catalog on the admin database.
            #
            # TODO SERVER-25493: Remove the $exists clause once performing an initial sync from
            # versions of MongoDB <= 3.2 is no longer supported.
            filter = {"$or": [filter, {"type": {"$exists": False}}]}

        # In a sharded cluster with in-progress validate command for the config database
        # (i.e. on the config server), a listCommand command on a mongos or shardsvr mongod that
        # has stale routing info may fail since a refresh would involve running read commands
        # against the config database. The read commands are lock free so they are not blocked by
        # the validate command and instead are subject to failing with a ObjectIsBusy error. Since
        # this is a transient state, we shoud retry.
        def list_collections(db: Database, filter: dict, timeout: int = 10):
            start_time = time.time()
            while time.time() - start_time < timeout:
                try:
                    return db.list_collection_names(filter=filter)
                except pymongo.errors.AutoReconnect:
                    self.logger.info("AutoReconnect exception thrown, retrying...")
                    time.sleep(0.1)
                except pymongo.errors.OperationFailure as ex:
                    # Error code 314 is 'ObjectIsBusy'
                    # https://www.mongodb.com/docs/manual/reference/error-codes/
                    if ex.code and ex.code == 314:
                        logger.warning(
                            f"Received ObjectIsBusy error when trying to find collections of {db_name}, retrying..."
                        )
                        time.sleep(0.1)
                        continue
                    raise
            raise RuntimeError(f"Timed out while trying to list collections for {db_name}")

        coll_names = list_collections(db, filter)

        for coll_name in coll_names:
            futures.append(
                executor.submit(
                    validate_collection,
                    db,
                    coll_name,
                    validate_opts,
                    skipValidationOnNamespaceNotFound,
                    logger,
                )
            )
        return True
    except:
        logger.exception("Unknown exception while validating database")
        return False


def validate_collection(
    db: Database,
    coll_name: str,
    validate_opts: dict,
    skipValidationOnNamespaceNotFound: bool,
    logger: logging.Logger,
):
    validate_cmd = {"validate": coll_name}
    validate_cmd.update(validate_opts)
    ret = db.command(validate_cmd, check=False)

    ok = "ok" in ret and ret["ok"]
    valid = "valid" in ret and ret["valid"]

    logger.info(f"Trying to validate collection {coll_name} in database {db.name}")

    if not ok or not valid:
        if (
            skipValidationOnNamespaceNotFound
            and "codeName" in ret
            and ret["codeName"] == "NamespaceNotFound"
        ):
            # During a 'stopStart' backup/restore on the secondary node, the actual list of
            # collections can be out of date if ops are still being applied from the oplog.
            # In this case we skip the collection if the ns was not found at time of
            # validation and continue to next.
            logger.info(
                f"Skipping collection validation for {coll_name} since collection was not found"
            )
            return True
        elif "codeName" in ret and ret["codeName"] == "CommandNotSupportedOnView":
            # Even though we pass a filter to getCollectionInfos() to only fetch
            # collections, nothing is preventing the collection from being dropped and
            # recreated as a view.
            logger.info(f"Skipping collection validation for {coll_name} as it is a view")
            return True

        # This message needs to include "collection validation failed" to match the
        # buildbaron search message
        logger.info(
            f"collection validation failed on collection {coll_name} in database {db.name} with response: {ret}"
        )
        dump_collection(db.get_collection(coll_name), 100, logger)
        return False

    logger.info(f"Collection validation passed on collection {coll_name} in database {db.name}")
    return True


def dump_collection(coll: Collection, limit: int, logger: logging.Logger):
    logger.info("Printing indexes in: " + coll.name)
    logger.info(coll.index_information())

    logger.info("Printing the first " + str(limit) + " documents in: " + coll.name)
    docs = coll.find().limit(limit)
    for doc in docs:
        logger.info(doc)
