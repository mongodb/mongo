"""
Customize the behavior of a fixture by allowing special code to be
executed before or after each test, and before or after each suite.
"""

from __future__ import absolute_import

import os
import sys

import pymongo

from . import fixtures
from . import testcases
from .. import errors
from .. import logging
from .. import utils


def make_custom_behavior(class_name, *args, **kwargs):
    """
    Factory function for creating CustomBehavior instances.
    """

    if class_name not in _CUSTOM_BEHAVIORS:
        raise ValueError("Unknown custom behavior class '%s'" % (class_name))
    return _CUSTOM_BEHAVIORS[class_name](*args, **kwargs)


class CustomBehavior(object):
    """
    The common interface all CustomBehaviors will inherit from.
    """

    @staticmethod
    def start_dynamic_test(test_case, test_report):
        """
        If a CustomBehavior wants to add a test case that will show up
        in the test report, it should use this method to add it to the
        report, since we will need to count it as a dynamic test to get
        the stats in the summary information right.
        """
        test_report.startTest(test_case, dynamic=True)

    def __init__(self, logger, fixture):
        """
        Initializes the CustomBehavior with the specified fixture.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        self.logger = logger
        self.fixture = fixture

    def before_suite(self, test_report):
        """
        The test runner calls this exactly once before they start
        running the suite.
        """
        pass

    def after_suite(self, test_report):
        """
        The test runner calls this exactly once after all tests have
        finished executing. Be sure to reset the behavior back to its
        original state so that it can be run again.
        """
        pass

    def before_test(self, test_report):
        """
        Each test will call this before it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass

    def after_test(self, test_report):
        """
        Each test will call this after it executes.

        Raises a TestFailure if the test should be marked as a failure,
        or a ServerFailure if the fixture exits uncleanly or
        unexpectedly.
        """
        pass


class CleanEveryN(CustomBehavior):
    """
    Restarts the fixture after it has ran 'n' tests.
    On mongod-related fixtures, this will clear the dbpath.
    """

    DEFAULT_N = 20

    def __init__(self, logger, fixture, n=DEFAULT_N):
        CustomBehavior.__init__(self, logger, fixture)

        # Try to isolate what test triggers the leak by restarting the fixture each time.
        if "detect_leaks=1" in os.getenv("ASAN_OPTIONS", ""):
            self.logger.info("ASAN_OPTIONS environment variable set to detect leaks, so restarting"
                             " the fixture after each test instead of after every %d.", n)
            n = 1

        self.n = n
        self.tests_run = 0

    def after_test(self, test_report):
        self.tests_run += 1
        if self.tests_run >= self.n:
            self.logger.info("%d tests have been run against the fixture, stopping it...",
                             self.tests_run)
            self.tests_run = 0

            teardown_success = self.fixture.teardown()
            self.logger.info("Starting the fixture back up again...")
            self.fixture.setup()
            self.fixture.await_ready()

            # Raise this after calling setup in case --continueOnFailure was specified.
            if not teardown_success:
                raise errors.TestFailure("%s did not exit cleanly" % (self.fixture))


class CheckReplDBHash(CustomBehavior):
    """
    Waits for replication after each test, then checks that the dbhahses
    of all databases other than "local" match on the primary and all of
    the secondaries. If any dbhashes do not match, logs information
    about what was different (e.g. Different numbers of collections,
    missing documents in a collection, mismatching documents, etc).

    Compatible only with ReplFixture subclasses.
    """

    def __init__(self, logger, fixture):
        if not isinstance(fixture, fixtures.ReplFixture):
            raise TypeError("%s does not support replication" % (fixture.__class__.__name__))

        CustomBehavior.__init__(self, logger, fixture)

        self.test_case = testcases.TestCase(self.logger, "Hook", "#dbhash#")

        self.started = False

    def after_test(self, test_report):
        """
        After each test, check that the dbhash of the test database is
        the same on all nodes in the replica set or master/slave
        fixture.
        """

        try:
            if not self.started:
                CustomBehavior.start_dynamic_test(self.test_case, test_report)
                self.started = True

            # Wait until all operations have replicated.
            self.fixture.await_repl()

            success = True
            sb = []  # String builder.

            primary = self.fixture.get_primary()
            primary_conn = utils.new_mongo_client(port=primary.port)

            for secondary in self.fixture.get_secondaries():
                read_preference = pymongo.ReadPreference.SECONDARY
                secondary_conn = utils.new_mongo_client(port=secondary.port,
                                                        read_preference=read_preference)
                # Skip arbiters.
                if secondary_conn.admin.command("isMaster").get("arbiterOnly", False):
                    continue

                all_matched = CheckReplDBHash._check_all_db_hashes(primary_conn,
                                                                   secondary_conn,
                                                                   sb)
                if not all_matched:
                    sb.insert(0,
                              "One or more databases were different between the primary on port %d"
                              " and the secondary on port %d:"
                              % (primary.port, secondary.port))

                success = all_matched and success

            if not success:
                # Adding failures to a TestReport requires traceback information, so we raise
                # a 'self.test_case.failureException' that we will catch ourselves.
                self.test_case.logger.info("\n    ".join(sb))
                raise self.test_case.failureException("The dbhashes did not match")
        except self.test_case.failureException as err:
            self.test_case.logger.exception("The dbhashes did not match.")
            self.test_case.return_code = 1
            test_report.addFailure(self.test_case, sys.exc_info())
            test_report.stopTest(self.test_case)
            raise errors.ServerFailure(err.message)
        except pymongo.errors.WTimeoutError:
            self.test_case.logger.exception("Awaiting replication timed out.")
            self.test_case.return_code = 2
            test_report.addError(self.test_case, sys.exc_info())
            test_report.stopTest(self.test_case)
            raise errors.StopExecution("Awaiting replication timed out")

    def after_suite(self, test_report):
        """
        If we get to this point, the #dbhash# test must have been
        successful, so add it to the test report.
        """

        if self.started:
            self.test_case.logger.exception("The dbhashes matched for all tests.")
            self.test_case.return_code = 0
            test_report.addSuccess(self.test_case)
            # TestReport.stopTest() has already been called if there was a failure.
            test_report.stopTest(self.test_case)

        self.started = False

    @staticmethod
    def _check_all_db_hashes(primary_conn, secondary_conn, sb):
        """
        Returns true if for each non-local database, the dbhash command
        returns the same MD5 hash on the primary as it does on the
        secondary. Returns false otherwise.

        Logs a message describing the differences if any database's
        dbhash did not match.
        """

        # Overview of how we'll check that everything replicated correctly between these two nodes:
        #
        # - Check whether they have the same databases.
        #     - If not, log which databases are missing where, and dump the contents of any that are
        #       missing.
        #
        # - Check whether each database besides "local" gives the same md5 field as the result of
        #   running the dbhash command.
        #     - If not, check whether they have the same collections.
        #         - If not, log which collections are missing where, and dump the contents of any
        #           that are missing.
        #     - If so, check that the hash of each non-capped collection matches.
        #         - If any do not match, log the diff of the collection between the two nodes.

        success = True

        if not CheckReplDBHash._check_dbs_present(primary_conn, secondary_conn, sb):
            return False

        for db_name in primary_conn.database_names():
            if db_name == "local":
                continue  # We don't expect this to match across different nodes.

            matched = CheckReplDBHash._check_db_hash(primary_conn, secondary_conn, db_name, sb)
            success = matched and success

        return success

    @staticmethod
    def _check_dbs_present(primary_conn, secondary_conn, sb):
        """
        Returns true if the list of databases on the primary is
        identical to the list of databases on the secondary, and false
        otherwise.
        """

        success = True
        primary_dbs = primary_conn.database_names()

        # Can't run database_names() on secondary, so instead use the listDatabases command.
        # TODO: Use database_names() once PYTHON-921 is resolved.
        list_db_output = secondary_conn.admin.command("listDatabases")
        secondary_dbs = [db["name"] for db in list_db_output["databases"]]

        missing_on_primary, missing_on_secondary = CheckReplDBHash._check_difference(
            set(primary_dbs), set(secondary_dbs), "database", sb)

        for missing_db in missing_on_secondary:
            db = primary_conn[missing_db]
            coll_names = db.collection_names()
            non_system_colls = [name for name in coll_names if not name.startswith("system.")]

            # It is only an error if there are any non-system collections in the database,
            # otherwise it's not well defined whether they should exist or not.
            if non_system_colls:
                sb.append("Database %s present on primary but not on secondary." % (missing_db))
                CheckReplDBHash._dump_all_collections(db, non_system_colls, sb)
                success = False

        for missing_db in missing_on_primary:
            db = secondary_conn[missing_db]

            # Can't run collection_names() on secondary, so instead use the listCollections command.
            # TODO: Always use collection_names() once PYTHON-921 is resolved. Then much of the
            # logic that is duplicated here can be consolidated.
            list_coll_output = db.command("listCollections")["cursor"]["firstBatch"]
            coll_names = [coll["name"] for coll in list_coll_output]
            non_system_colls = [name for name in coll_names if not name.startswith("system.")]

            # It is only an error if there are any non-system collections in the database,
            # otherwise it's not well defined if it should exist or not.
            if non_system_colls:
                CheckReplDBHash._dump_all_collections(db, non_system_colls, sb)
                success = False

        return success

    @staticmethod
    def _check_db_hash(primary_conn, secondary_conn, db_name, sb):
        """
        Returns true if the dbhash for 'db_name' matches on the primary
        and the secondary, and false otherwise.

        Appends a message to 'sb' describing the differences if the
        dbhashes do not match.
        """

        primary_hash = primary_conn[db_name].command("dbhash")
        secondary_hash = secondary_conn[db_name].command("dbhash")

        if primary_hash["md5"] == secondary_hash["md5"]:
            return True

        success = CheckReplDBHash._check_dbs_eq(
            primary_conn, secondary_conn, primary_hash, secondary_hash, db_name, sb)

        if not success:
            sb.append("Database %s has a different hash on the primary and the secondary"
                      " ([ %s ] != [ %s ]):"
                      % (db_name, primary_hash["md5"], secondary_hash["md5"]))

        return success

    @staticmethod
    def _check_dbs_eq(primary_conn, secondary_conn, primary_hash, secondary_hash, db_name, sb):
        """
        Returns true if all non-capped collections had the same hash in
        the dbhash response, and false otherwise.

        Appends information to 'sb' about the differences between the
        'db_name' database on the primary and the 'db_name' database on
        the secondary, if any.
        """

        success = True

        primary_db = primary_conn[db_name]
        secondary_db = secondary_conn[db_name]

        primary_coll_hashes = primary_hash["collections"]
        secondary_coll_hashes = secondary_hash["collections"]

        primary_coll_names = set(primary_coll_hashes.keys())
        secondary_coll_names = set(secondary_coll_hashes.keys())

        missing_on_primary, missing_on_secondary = CheckReplDBHash._check_difference(
            primary_coll_names, secondary_coll_names, "collection", sb)

        if missing_on_primary or missing_on_secondary:

            # 'sb' already describes which collections are missing where.
            for coll_name in missing_on_primary:
                CheckReplDBHash._dump_all_documents(primary_db, coll_name, sb)
            for coll_name in missing_on_secondary:
                CheckReplDBHash._dump_all_documents(secondary_db, coll_name, sb)
            return

        for coll_name in primary_coll_names & secondary_coll_names:
            primary_coll_hash = primary_coll_hashes[coll_name]
            secondary_coll_hash = secondary_coll_hashes[coll_name]

            if primary_coll_hash == secondary_coll_hash:
                continue

            # Ignore capped collections because they are not expected to match on all nodes.
            if primary_db.command({"collStats": coll_name})["capped"]:
                # Still fail if the collection is not capped on the secondary.
                if not secondary_db.command({"collStats": coll_name})["capped"]:
                    success = False
                    sb.append("%s.%s collection is capped on primary but not on secondary."
                              % (primary_db.name, coll_name))
                sb.append("%s.%s collection is capped, ignoring." % (primary_db.name, coll_name))
                continue
            # Still fail if the collection is capped on the secondary, but not on the primary.
            elif secondary_db.command({"collStats": coll_name})["capped"]:
                success = False
                sb.append("%s.%s collection is capped on secondary but not on primary."
                          % (primary_db.name, coll_name))
                continue

            success = False
            sb.append("Collection %s.%s has a different hash on the primary and the secondary"
                      " ([ %s ] != [ %s ]):"
                      % (db_name, coll_name, primary_coll_hash, secondary_coll_hash))
            CheckReplDBHash._check_colls_eq(primary_db, secondary_db, coll_name, sb)

        if success:
            sb.append("All collections that were expected to match did.")
        return success

    @staticmethod
    def _check_colls_eq(primary_db, secondary_db, coll_name, sb):
        """
        Appends information to 'sb' about the differences or between
        the 'coll_name' collection on the primary and the 'coll_name'
        collection on the secondary, if any.
        """

        primary_coll = primary_db[coll_name]
        secondary_coll = secondary_db[coll_name]

        primary_docs = CheckReplDBHash._extract_documents(primary_coll)
        secondary_docs = CheckReplDBHash._extract_documents(secondary_coll)

        CheckReplDBHash._get_collection_diff(primary_docs, secondary_docs, sb)

    @staticmethod
    def _extract_documents(collection):
        """
        Returns a list of all documents in the collection, sorted by
        their _id.
        """

        return [doc for doc in collection.find().sort("_id", pymongo.ASCENDING)]

    @staticmethod
    def _get_collection_diff(primary_docs, secondary_docs, sb):
        """
        Returns true if the documents in 'primary_docs' exactly match
        the documents in 'secondary_docs', and false otherwise.

        Appends information to 'sb' about what matched or did not match.
        """

        matched = True

        # These need to be lists instead of sets because documents aren't hashable.
        missing_on_primary = []
        missing_on_secondary = []

        p_idx = 0  # Keep track of our position in 'primary_docs'.
        s_idx = 0  # Keep track of our position in 'secondary_docs'.

        while p_idx < len(primary_docs) and s_idx < len(secondary_docs):
            primary_doc = primary_docs[p_idx]
            secondary_doc = secondary_docs[s_idx]

            if primary_doc == secondary_doc:
                p_idx += 1
                s_idx += 1
                continue

            # We have mismatching documents.
            matched = False

            if primary_doc["_id"] == secondary_doc["_id"]:
                sb.append("Mismatching document:")
                sb.append("    primary:   %s" % (primary_doc))
                sb.append("    secondary: %s" % (secondary_doc))

            # One node was missing a document. Since the documents are sorted by _id, the doc with
            # the smaller _id was the one that was skipped.
            elif primary_doc["_id"] < secondary_doc["_id"]:
                missing_on_secondary.append(primary_doc)

                # Only move past the doc that we know was skipped.
                p_idx += 1

            else:  # primary_doc["_id"] > secondary_doc["_id"]
                missing_on_primary.append(secondary_doc)

                # Only move past the doc that we know was skipped.
                s_idx += 1

        # Check if there are any unmatched documents left.
        while p_idx < len(primary_docs):
            matched = False
            missing_on_secondary.append(primary_docs[p_idx])
            p_idx += 1
        while s_idx < len(secondary_docs):
            matched = False
            missing_on_primary.append(secondary_docs[s_idx])
            s_idx += 1

        if not matched:
            CheckReplDBHash._append_differences(
                missing_on_primary, missing_on_secondary, "document", sb)
        else:
            sb.append("All documents matched.")

    @staticmethod
    def _check_difference(primary_set, secondary_set, item_type_name, sb):
        """
        Returns true if the contents of 'primary_set' and
        'secondary_set' are identical, and false otherwise. The sets
        contain information about the primary and secondary,
        respectively, e.g. the database names that exist on each node.

        Appends information about anything that differed to 'sb'.
        """

        missing_on_primary = set()
        missing_on_secondary = set()

        for item in primary_set - secondary_set:
            missing_on_secondary.add(item)

        for item in secondary_set - primary_set:
            missing_on_primary.add(item)

        CheckReplDBHash._append_differences(
            missing_on_primary, missing_on_secondary, item_type_name, sb)

        return (missing_on_primary, missing_on_secondary)

    @staticmethod
    def _append_differences(missing_on_primary, missing_on_secondary, item_type_name, sb):
        """
        Given two iterables representing items that were missing on the
        primary or the secondary respectively, append the information
        about which items were missing to 'sb', if any.
        """

        if missing_on_primary:
            sb.append("The following %ss were present on the secondary, but not on the"
                      " primary:" % (item_type_name))
            for item in missing_on_primary:
                sb.append(str(item))

        if missing_on_secondary:
            sb.append("The following %ss were present on the primary, but not on the"
                      " secondary:" % (item_type_name))
            for item in missing_on_secondary:
                sb.append(str(item))

    @staticmethod
    def _dump_all_collections(database, coll_names, sb):
        """
        Appends the contents of each of the collections in 'coll_names'
        to 'sb'.
        """

        if coll_names:
            sb.append("Database %s contains the following collections: %s"
                      % (database.name, coll_names))
            for coll_name in coll_names:
                CheckReplDBHash._dump_all_documents(database, coll_name, sb)
        else:
            sb.append("No collections in database %s." % (database.name))

    @staticmethod
    def _dump_all_documents(database, coll_name, sb):
        """
        Appends the contents of 'coll_name' to 'sb'.
        """

        docs = CheckReplDBHash._extract_documents(database[coll_name])
        if docs:
            sb.append("Documents in %s.%s:" % (database.name, coll_name))
            for doc in docs:
                sb.append("    %s" % (doc))
        else:
            sb.append("No documents in %s.%s." % (database.name, coll_name))


_CUSTOM_BEHAVIORS = {
    "CleanEveryN": CleanEveryN,
    "CheckReplDBHash": CheckReplDBHash,
}
