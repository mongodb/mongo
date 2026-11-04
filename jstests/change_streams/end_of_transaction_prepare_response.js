/**
 * Tests that a 'prepareTransaction' response includes the 'affectedNamespaces' field when the
 * end-of-transaction synthesis feature flag is enabled. The coordinator relies on these namespaces
 * to build the commit-time accounting metadata.
 *
 * Covers single- and multi-namespace transactions (multiple collections and multiple databases),
 * and transactions that are committed as well as transactions that are aborted after preparing.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   # 'prepareTransaction' is a mongod command and is not available on mongos, so the test can
 *   # only run against a replica set or a shard server, not through a router.
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

describe("prepareTransaction affectedNamespaces", function () {
    const dbName = jsTestName();
    const otherDbName = dbName + "_other";
    const collName = "prepare_response_affected_namespaces";
    const otherCollName = "prepare_response_affected_namespaces_2";

    const testDb = db.getSiblingDB(dbName);
    const otherDb = db.getSiblingDB(otherDbName);

    before(function () {
        assertDropCollection(testDb, collName);
        assertDropCollection(testDb, otherCollName);
        assertDropCollection(otherDb, collName);

        assert.commandWorked(testDb.getCollection(collName).insert({_id: 0}));
        assert.commandWorked(testDb.getCollection(otherCollName).insert({_id: 0}));
        assert.commandWorked(otherDb.getCollection(collName).insert({_id: 0}));
    });

    after(function () {
        assertDropCollection(testDb, collName);
        assertDropCollection(testDb, otherCollName);
        assertDropCollection(otherDb, collName);
    });

    // Prepares the active transaction on the session and returns the raw 'prepareTransaction'
    // response.
    function prepareAndGetResponse(session) {
        return assert.commandWorked(
            session
                .getDatabase("admin")
                .adminCommand({prepareTransaction: 1, writeConcern: {w: "majority"}}),
        );
    }

    // Asserts that the prepare response carries a 'prepareTimestamp' and exactly the expected
    // namespaces, in any order.
    function assertAffectedNamespaces(prepareResponse, expectedNamespaces) {
        assert(
            prepareResponse.prepareTimestamp,
            "prepareTransaction did not return a prepareTimestamp",
            {prepareResponse},
        );
        assert(
            prepareResponse.affectedNamespaces,
            "prepareTransaction did not return affectedNamespaces",
            {prepareResponse},
        );
        assert.sameMembers(
            expectedNamespaces,
            prepareResponse.affectedNamespaces,
            "unexpected affectedNamespaces",
            undefined,
            {prepareResponse},
        );
    }

    it("returns the namespaces written by the transaction", function () {
        const prepareResponse = withSession(db.getMongo(), (session) => {
            const sessionColl = session.getDatabase(dbName).getCollection(collName);
            session.startTransaction();
            assert.commandWorked(sessionColl.insert({_id: 1}));

            const res = prepareAndGetResponse(session);
            assert.commandWorked(session.abortTransaction_forTesting());
            return res;
        });

        assertAffectedNamespaces(prepareResponse, [`${dbName}.${collName}`]);
    });

    it("returns the namespaces of a transaction that is committed", function () {
        let prepareResponse;
        withSession(db.getMongo(), (session) => {
            const sessionColl = session.getDatabase(dbName).getCollection(collName);
            session.startTransaction();
            assert.commandWorked(sessionColl.insert({_id: 2}));

            prepareResponse = prepareAndGetResponse(session);
            // Commit the prepared transaction instead of aborting it.
            assert.commandWorked(
                PrepareHelpers.commitTransaction(session, prepareResponse.prepareTimestamp),
            );

            // The transaction was committed, so the write is visible. Read on the same session so
            // that the read is causally consistent with the commit and therefore observes it even
            // when the read is served from a secondary.
            assert.eq(1, sessionColl.countDocuments({_id: 2}));
        });

        assertAffectedNamespaces(prepareResponse, [`${dbName}.${collName}`]);
    });

    it("returns every namespace written across multiple collections", function () {
        let prepareResponse;
        withSession(db.getMongo(), (session) => {
            const sessionDb = session.getDatabase(dbName);
            session.startTransaction();
            assert.commandWorked(sessionDb.getCollection(collName).insert({_id: 3}));
            assert.commandWorked(sessionDb.getCollection(otherCollName).insert({_id: 3}));

            prepareResponse = prepareAndGetResponse(session);
            assert.commandWorked(
                PrepareHelpers.commitTransaction(session, prepareResponse.prepareTimestamp),
            );

            // Read on the same session so that the reads are causally consistent with the commit
            // and therefore observe it even when served from a secondary.
            assert.eq(1, sessionDb.getCollection(collName).countDocuments({_id: 3}));
            assert.eq(1, sessionDb.getCollection(otherCollName).countDocuments({_id: 3}));
        });

        assertAffectedNamespaces(prepareResponse, [
            `${dbName}.${collName}`,
            `${dbName}.${otherCollName}`,
        ]);
    });

    it("returns every namespace written across multiple databases", function () {
        let prepareResponse;
        withSession(db.getMongo(), (session) => {
            session.startTransaction();
            assert.commandWorked(
                session.getDatabase(dbName).getCollection(collName).insert({_id: 4}),
            );
            assert.commandWorked(
                session.getDatabase(otherDbName).getCollection(collName).insert({_id: 4}),
            );

            prepareResponse = prepareAndGetResponse(session);
            assert.commandWorked(
                PrepareHelpers.commitTransaction(session, prepareResponse.prepareTimestamp),
            );

            // Read on the same session so that the reads are causally consistent with the commit
            // and therefore observe it even when served from a secondary.
            assert.eq(
                1,
                session.getDatabase(dbName).getCollection(collName).countDocuments({_id: 4}),
            );
            assert.eq(
                1,
                session.getDatabase(otherDbName).getCollection(collName).countDocuments({_id: 4}),
            );
        });

        assertAffectedNamespaces(prepareResponse, [
            `${dbName}.${collName}`,
            `${otherDbName}.${collName}`,
        ]);
    });
});
