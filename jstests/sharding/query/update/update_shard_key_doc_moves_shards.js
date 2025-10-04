/**
 * Tests that changing the shard key value of a document using update and findAndModify works
 * correctly when the doc will change shards.
 * @tags: [
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

import {withAbortAndRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    enableCoordinateCommitReturnImmediatelyAfterPersistingDecision,
    isUpdateDocumentShardKeyUsingTransactionApiEnabled,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {
    assertCannotUpdate_id,
    assertCannotUpdate_idDottedPath,
    assertCannotUpdateInBulkOpWhenDocsMoveShards,
    assertCannotUpdateSKToArray,
    assertCannotUpdateWithMultiTrue,
    assertCanUnsetSKField,
    assertCanUpdateDottedPath,
    assertCanUpdatePartialShardKey,
    assertCanUpdatePrimitiveShardKey,
    assertCanUpdatePrimitiveShardKeyHashedChangeShards,
    runFindAndModifyCmdFail,
    runFindAndModifyCmdSuccess,
    runUpdateCmdFail,
    runUpdateCmdSuccess,
    shardCollectionMoveChunks,
} from "jstests/sharding/libs/update_shard_key_helpers.js";

const st = new ShardingTest({
    mongos: 1,
    shards: {rs0: {nodes: 3}, rs1: {nodes: 3}},
    rsOptions: {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}},
});

const kDbName = "db";
const mongos = st.s0;
const shard0 = st.shard0.shardName;
const ns = kDbName + ".foo";

const updateDocumentShardKeyUsingTransactionApiEnabled = isUpdateDocumentShardKeyUsingTransactionApiEnabled(st.s);

enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function changeShardKeyWhenFailpointsSet(
    session,
    sessionDB,
    runInTxn,
    isFindAndModify,
    updateDocumentShardKeyUsingTransactionApiEnabled,
) {
    const docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    const splitDoc = {x: 100};
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, splitDoc, {"x": 300});

    // Assert that the document is updated when the delete fails.
    assert.commandWorked(
        st.rs1
            .getPrimary()
            .getDB(kDbName)
            .adminCommand({
                configureFailPoint: "failCommand",
                mode: {times: 2},
                data: {
                    errorCode: ErrorCodes.WriteConflict,
                    failCommands: ["delete"],
                    failInternalCommands: true,
                },
            }),
    );
    if (isFindAndModify) {
        if (!runInTxn && updateDocumentShardKeyUsingTransactionApiEnabled) {
            // Internal transactions will retry internally with the transaction API.
            runFindAndModifyCmdSuccess(
                st,
                kDbName,
                session,
                sessionDB,
                runInTxn,
                [{x: 300}],
                [{$set: {x: 30}}],
                false /* upsert */,
                false /* returnNew */,
                splitDoc,
            );
        } else {
            runFindAndModifyCmdFail(st, kDbName, session, sessionDB, runInTxn, {x: 300}, {$set: {x: 30}}, false);
        }
    } else {
        if (!runInTxn && updateDocumentShardKeyUsingTransactionApiEnabled) {
            // Internal transactions will retry internally with the transaction API.
            runUpdateCmdSuccess(
                st,
                kDbName,
                session,
                sessionDB,
                runInTxn,
                [{x: 300}],
                [{$set: {x: 30}}],
                false /* upsert */,
                splitDoc,
            );
        } else {
            runUpdateCmdFail(
                st,
                kDbName,
                session,
                sessionDB,
                runInTxn,
                {x: 300},
                {$set: {x: 30}},
                false,
                ErrorCodes.WriteConflict,
            );
        }
    }
    assert.commandWorked(
        st.rs1.getPrimary().getDB(kDbName).adminCommand({
            configureFailPoint: "failCommand",
            mode: "off",
        }),
    );

    // Reset the collection's documents.
    assert.commandWorked(st.s.getDB(kDbName).foo.remove({}));
    assert.commandWorked(st.s.getDB(kDbName).foo.insert(docsToInsert));

    // Assert that the document is not updated when the insert fails for a non transient reason.
    assert.commandWorked(
        st.rs0
            .getPrimary()
            .getDB(kDbName)
            .adminCommand({
                configureFailPoint: "failCommand",
                mode: "alwaysOn",
                data: {
                    errorCode: ErrorCodes.NamespaceNotFound,
                    failCommands: ["insert"],
                    failInternalCommands: true,
                },
            }),
    );
    if (isFindAndModify) {
        runFindAndModifyCmdFail(st, kDbName, session, sessionDB, runInTxn, {"x": 300}, {"$set": {"x": 30}}, false);
    } else {
        runUpdateCmdFail(
            st,
            kDbName,
            session,
            sessionDB,
            runInTxn,
            {"x": 300},
            {"$set": {"x": 30}},
            false,
            ErrorCodes.NamespaceNotFound,
        );
    }
    assert.commandWorked(
        st.rs0.getPrimary().getDB(kDbName).adminCommand({
            configureFailPoint: "failCommand",
            mode: "off",
        }),
    );

    // Reset the collection's documents.
    assert.commandWorked(st.s.getDB(kDbName).foo.remove({}));
    assert.commandWorked(st.s.getDB(kDbName).foo.insert(docsToInsert));

    // Assert that the shard key update is not committed when there are no write errors and the
    // transaction is explicity aborted.
    if (runInTxn) {
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction();
            if (isFindAndModify) {
                sessionDB.foo.findAndModify({query: {"x": 300}, update: {"$set": {"x": 30}}});
            } else {
                assert.commandWorked(sessionDB.foo.update({"x": 300}, {"$set": {"x": 30}}));
            }
            assert.commandWorked(session.abortTransaction_forTesting());
        });
        assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 300}).itcount());
        assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
    }

    mongos.getDB(kDbName).foo.drop();
}

//
// Test that changing the shard key works correctly when either the update or findAndModify
// command is used and when the command is run either as a retryable write or in a transaction.
// Tuples represent [shouldRunCommandInTxn, runUpdateAsFindAndModifyCmd, isUpsert].
//

const changeShardKeyOptions = [
    [false, false, false],
    [true, false, false],
    [true, true, false],
    [false, true, false],
    [false, false, true],
    [true, false, true],
    [false, true, true],
    [true, true, true],
];

//
//  Tests for op-style updates.
//

changeShardKeyOptions.forEach(function (updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog(
        "Testing changing the shard key using op style update and " +
            (isFindAndModify ? "findAndModify command " : "update command ") +
            (runInTxn ? "in transaction " : "as retryable write"),
    );

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300}, {"x": 4}],
        [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
        upsert,
    );
    assertCanUpdateDottedPath(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x.a": 300}, {"x.a": 4}],
        [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
        upsert,
    );
    assertCanUpdatePartialShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [
            {"x": 300, "y": 80},
            {"x": 4, "y": 3},
        ],
        [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
        upsert,
    );

    assertCanUnsetSKField(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        {"x": 300},
        {"$unset": {"x": 1}},
        upsert,
    );

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"_id": 300},
            {
                "$set": {"_id": 30},
            },
        );
        assertCannotUpdate_idDottedPath(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"_id.a": 300},
            {
                "$set": {"_id": {"a": 30}},
            },
        );
        assertCannotUpdateSKToArray(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"x": 300},
            {
                "$set": {"x": [30]},
            },
        );

        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(
                st,
                kDbName,
                ns,
                session,
                sessionDB,
                runInTxn,
                {"x": 300},
                {"$set": {"x": 30}},
            );
        }

        changeShardKeyWhenFailpointsSet(
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            updateDocumentShardKeyUsingTransactionApiEnabled,
        );
    }
});

//
// Tests for replacement style updates.
//

changeShardKeyOptions.forEach(function (updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog(
        "Testing changing the shard key using replacement style update and " +
            (isFindAndModify ? "findAndModify command " : "update command ") +
            (runInTxn ? "in transaction " : "as retryable write"),
    );

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x": 300}, {"x": 4}],
        [{"x": 30}, {"x": 600}],
        upsert,
    );
    assertCanUpdateDottedPath(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [{"x.a": 300}, {"x.a": 4}],
        [{"x": {"a": 30}}, {"x": {"a": 600}}],
        upsert,
    );
    assertCanUpdatePartialShardKey(
        st,
        kDbName,
        ns,
        session,
        sessionDB,
        runInTxn,
        isFindAndModify,
        [
            {"x": 300, "y": 80},
            {"x": 4, "y": 3},
        ],
        [
            {"x": 30, "y": 80},
            {"x": 600, "y": 3},
        ],
        upsert,
    );

    assertCanUnsetSKField(st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"x": 300}, {}, upsert);

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"_id": 300},
            {
                "_id": 30,
            },
        );
        assertCannotUpdate_idDottedPath(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"_id.a": 300},
            {
                "_id": {"a": 30},
            },
        );
        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(st, kDbName, ns, session, sessionDB, runInTxn, {"x": 300}, {"x": 30});
        }
        assertCannotUpdateSKToArray(
            st,
            kDbName,
            ns,
            session,
            sessionDB,
            runInTxn,
            isFindAndModify,
            {"x": 300},
            {
                "x": [30],
            },
        );
    }
});

let session = st.s.startSession({retryWrites: true});
let sessionDB = session.getDatabase(kDbName);

let docsToInsert = [{"x": 4, "a": 3}, {"x": 78}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];

// ----Assert correct behavior when collection is hash sharded----

// Non-upsert case
assertCanUpdatePrimitiveShardKeyHashedChangeShards(st, kDbName, ns, session, sessionDB, false);
assertCanUpdatePrimitiveShardKeyHashedChangeShards(st, kDbName, ns, session, sessionDB, true);

// ----Assert correct error when changing a doc shard key conflicts with an orphan----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
mongos.getDB(kDbName).foo.insert({"x": 505});

let _id = mongos.getDB(kDbName).foo.find({"x": 505}).toArray()[0]._id;
assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).foo.insert({"x": 2, "_id": _id}));

let res = sessionDB.foo.update({"x": 505}, {"$set": {"x": 20}});
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert(
    res
        .getWriteError()
        .errmsg.includes(
            "There is either an orphan for this document or _id for this collection is not globally unique.",
        ),
);

withAbortAndRetryOnTransientTxnError(session, () => {
    session.startTransaction();
    res = sessionDB.foo.update({"x": 505}, {"$set": {"x": 20}});
    assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
});
assert(
    res.errmsg.includes(
        "There is either an orphan for this document or _id for this collection is not globally unique.",
    ),
);
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

mongos.getDB(kDbName).foo.drop();

// ----Assert that specifying writeConcern succeeds----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

assert.commandWorked(sessionDB.foo.update({x: 4}, {$set: {x: 1000}}, {writeConcern: {w: 1}}));

assert.commandWorked(
    sessionDB.runCommand({
        findAndModify: "foo",
        query: {x: 78},
        update: {$set: {x: 250}},
        lsid: {id: UUID()},
        txnNumber: NumberLong(1),
        writeConcern: {w: 1},
    }),
);

mongos.getDB(kDbName).foo.drop();

// ----Assert retryable write result has WCE when the internal commitTransaction fails----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

// Turn on failcommand fail point to fail CoordinateCommitTransaction
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB(kDbName)
        .adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                writeConcernError: {code: NumberInt(12345), errmsg: "dummy error"},
                failCommands: ["coordinateCommitTransaction"],
                failInternalCommands: true,
            },
        }),
);

res = sessionDB.foo.update({x: 4}, {$set: {x: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(12345, res.getWriteConcernError().code);

res = sessionDB.runCommand({
    findAndModify: "foo",
    query: {x: 78},
    update: {$set: {x: 250}},
    lsid: {id: UUID()},
    txnNumber: NumberLong(1),
});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(res.writeConcernError.code, 12345);
assert(res.writeConcernError.errmsg.includes("dummy error"));

assert.commandWorked(
    st.rs0.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }),
);

mongos.getDB(kDbName).foo.drop();

// ----Assert write result reports error when the internal transaction fails to commit----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

// Set this failpoint on the coordinator shard so that sending prepareTransaction to
// participating shards will fail.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB(kDbName)
        .adminCommand({
            configureFailPoint: "failRemoteTransactionCommand",
            mode: "alwaysOn",
            data: {command: "prepareTransaction", code: ErrorCodes.TransactionTooOld},
        }),
);

res = sessionDB.foo.update({x: 4}, {$set: {x: 1000}});

// For update, we should have reported the error in the write errors field and the top-level
// fields should reflect that no update was performed.
assert.writeErrorWithCode(res, ErrorCodes.TransactionTooOld);
assert.eq(res.nModified, 0);
assert.eq(res.nMatched, 0);
assert.eq(res.nUpserted, 0);

res = sessionDB.runCommand({
    findAndModify: "foo",
    query: {x: 78},
    update: {$set: {x: 250}},
    lsid: {id: UUID()},
    txnNumber: NumberLong(1),
});

// findAndModify reports the failure as a top-level error.
assert.commandFailedWithCode(res, ErrorCodes.TransactionTooOld);

assert.commandWorked(
    st.rs0.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failRemoteTransactionCommand",
        mode: "off",
    }),
);

mongos.getDB(kDbName).foo.drop();

// ----Assert that updating the shard key in a batch with size > 1 fails----

assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, false, true);
assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, false, false);

session = st.s.startSession({retryWrites: false});
sessionDB = session.getDatabase(kDbName);
assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, true, true);
assertCannotUpdateInBulkOpWhenDocsMoveShards(st, kDbName, ns, session, sessionDB, true, false);

// ----Multiple writes in txn-----

// Update two docs, updating one twice
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

let id;
withAbortAndRetryOnTransientTxnError(session, () => {
    session.startTransaction();
    id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
    assert.commandWorked(sessionDB.foo.update({"x": 30}, {"$set": {"x": 600}}));
    assert.commandWorked(sessionDB.foo.update({"x": 4}, {"$set": {"x": 50}}));
    assert.commandWorked(session.commitTransaction_forTesting());
});

assert.eq(0, sessionDB.foo.find({"x": 500}).itcount());
assert.eq(0, sessionDB.foo.find({"x": 30}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 600}).itcount());
assert.eq(id, sessionDB.foo.find({"x": 600}).toArray()[0]._id);
assert.eq(0, sessionDB.foo.find({"x": 4}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 50}).itcount());

mongos.getDB(kDbName).foo.drop();

// Check that doing $inc on doc A, then updating shard key for doc A, then $inc again only incs
// once
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

withAbortAndRetryOnTransientTxnError(session, () => {
    session.startTransaction();
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    assert.commandWorked(session.commitTransaction_forTesting());
});

assert.eq(0, sessionDB.foo.find({"x": 500}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 30}).itcount());
assert.eq(1, sessionDB.foo.find({"a": 7}).itcount());
assert.eq(1, sessionDB.foo.find({"x": 30, "a": 7}).itcount());

mongos.getDB(kDbName).foo.drop();

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});

// Insert and $inc before moving doc
withAbortAndRetryOnTransientTxnError(session, () => {
    session.startTransaction();
    id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
    assert.commandWorked(sessionDB.foo.insert({"x": 1, "a": 1}));
    assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
    sessionDB.foo.findAndModify({query: {"x": 500}, update: {$set: {"x": 20}}});
    assert.commandWorked(session.commitTransaction_forTesting());
});

assert.eq(0, sessionDB.foo.find({"x": 500}).toArray().length);
assert.eq(1, sessionDB.foo.find({"x": 20}).toArray().length);
assert.eq(20, sessionDB.foo.find({"_id": id}).toArray()[0].x);
assert.eq(1, sessionDB.foo.find({"a": 7}).toArray().length);
assert.eq(1, sessionDB.foo.find({"x": 20, "a": 7}).toArray().length);
assert.eq(1, sessionDB.foo.find({"x": 1}).toArray().length);

mongos.getDB(kDbName).foo.drop();

st.stop();
