/**
 * Tests that a change stream correctly generates an endOfTransaction event for a multi-document
 * transaction that contains DDL operations (create and createIndexes), and does not trigger
 * tassert 7694300.
 *
 * @tags: [
 *   uses_transactions,
 *   requires_fcv_71,
 *   requires_majority_read_concern,
 *   featureFlagEndOfTransactionChangeEvent,
 *   # Sharding a collection in a transaction is not allowed, and creating an index in a transaction
 *   # on a collection that was not created in that transaction is also not allowed.
 *   assumes_unsharded_collection,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertEndOfTransaction, ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("endOfTransaction event with DDL operations in a transaction", () => {
    const dbName = jsTestName() + "_db";
    const collName = jsTestName() + "_coll";

    const testDb = db.getSiblingDB(dbName);

    let cst;

    beforeEach(() => {
        assertDropCollection(testDb, collName);
    });

    afterEach(() => {
        assertDropCollection(testDb, collName);

        if (cst) {
            cst.cleanUp();
        }
        cst = null;
    });

    it("generates create, createIndexes, and endOfTransaction events without triggering tassert 7694300", () => {
        cst = new ChangeStreamTest(testDb);
        const changeStream = cst.startWatchingChanges({
            pipeline: [
                {$changeStream: {showExpandedEvents: true}},
                {$project: {"lsid.uid": 0, "operationDescription.lsid.uid": 0}},
            ],
            collection: collName,
            doNotModifyInPassthroughs: true,
        });

        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDb = session.getDatabase(dbName);
        const sessionColl = sessionDb[collName];

        const txnOptions = {
            readConcern: {level: "local"},
            writeConcern: {w: "majority"},
        };

        withTxnAndAutoRetryOnMongos(
            session,
            () => {
                assert.commandWorked(sessionColl.createIndex({x: 1}));
            },
            txnOptions,
        );

        const txnNumber = session.getTxnNumber_forTesting();
        const lsid = session.getSessionId();
        session.endSession();

        const expectedChanges = [
            {
                operationType: "create",
                ns: {db: dbName, coll: collName},
            },
            {
                operationType: "createIndexes",
                ns: {db: dbName, coll: collName},
                operationDescription: {indexes: [{v: 2, key: {x: 1}, name: "x_1"}]},
                lsid,
                txnNumber,
            },
            {
                operationType: "endOfTransaction",
                operationDescription: {lsid, txnNumber},
                lsid,
                txnNumber,
            },
        ];

        const changes = cst.assertNextChangesEqualWithDeploymentAwareness({
            cursor: changeStream,
            expectedChanges,
        });
        assertEndOfTransaction(changes);
    });
});
