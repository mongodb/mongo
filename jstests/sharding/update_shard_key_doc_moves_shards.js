/*
 * Tests that changing the shard key value of a document using update and findAndModify works
 * correctly when the doc will change shards.
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
'use strict';

load("jstests/sharding/libs/update_shard_key_helpers.js");

const st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});
const kDbName = 'db';
const mongos = st.s0;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const ns = kDbName + '.foo';

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, shard0);

function changeShardKeyWhenFailpointsSet(session, sessionDB, runInTxn, isFindAndModify) {
    let docsToInsert = [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
    cleanupOrphanedDocs(st, ns);

    // Assert that the document is not updated when the delete fails
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.WriteConflict,
            failCommands: ["delete"],
            failInternalCommands: true
        }
    }));
    if (isFindAndModify) {
        runFindAndModifyCmdFail(
            st, kDbName, session, sessionDB, runInTxn, {"x": 300}, {"$set": {"x": 30}}, false);
    } else {
        runUpdateCmdFail(st,
                         kDbName,
                         session,
                         sessionDB,
                         runInTxn,
                         {"x": 300},
                         {"$set": {"x": 30}},
                         false,
                         ErrorCodes.WriteConflict);
    }
    assert.commandWorked(st.rs1.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the document is not updated when the insert fails
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.NamespaceNotFound,
            failCommands: ["insert"],
            failInternalCommands: true
        }
    }));
    if (isFindAndModify) {
        runFindAndModifyCmdFail(
            st, kDbName, session, sessionDB, runInTxn, {"x": 300}, {"$set": {"x": 30}}, false);
    } else {
        runUpdateCmdFail(st,
                         kDbName,
                         session,
                         sessionDB,
                         runInTxn,
                         {"x": 300},
                         {"$set": {"x": 30}},
                         false,
                         ErrorCodes.NamespaceNotFound);
    }
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "failCommand",
        mode: "off",
    }));

    // Assert that the shard key update is not committed when there are no write errors and the
    // transaction is explicity aborted.
    if (runInTxn) {
        session.startTransaction();
        if (isFindAndModify) {
            sessionDB.foo.findAndModify({query: {"x": 300}, update: {"$set": {"x": 30}}});
        } else {
            assert.commandWorked(sessionDB.foo.update({"x": 300}, {"$set": {"x": 30}}));
        }
        assert.commandWorked(session.abortTransaction_forTesting());
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
    [true, true, true]
];

//
//  Tests for op-style updates.
//

changeShardKeyOptions.forEach(function(updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog("Testing changing the shard key using op style update and " +
              (isFindAndModify ? "findAndModify command " : "update command ") +
              (runInTxn ? "in transaction " : "as retryable write"));

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     runInTxn,
                                     isFindAndModify,
                                     [{"x": 300}, {"x": 4}],
                                     [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                     upsert);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"$set": {"x": {"a": 30}}}, {"$set": {"x": {"a": 600}}}],
                              upsert);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   runInTxn,
                                   isFindAndModify,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"$set": {"x": 30}}, {"$set": {"x": 600}}],
                                   upsert);

    assertCanUnsetSKField(st,
                          kDbName,
                          ns,
                          session,
                          sessionDB,
                          runInTxn,
                          isFindAndModify,
                          {"x": 300},
                          {"$unset": {"x": 1}},
                          upsert);

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"_id": 300}, {
                "$set": {"_id": 30}
            });
        assertCannotUpdate_idDottedPath(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"_id.a": 300}, {
                "$set": {"_id": {"a": 30}}
            });
        assertCannotUpdateSKToArray(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"x": 300}, {
                "$set": {"x": [30]}
            });

        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(
                st, kDbName, ns, session, sessionDB, runInTxn, {"x": 300}, {"$set": {"x": 30}});
        }
        changeShardKeyWhenFailpointsSet(session, sessionDB, runInTxn, isFindAndModify);
    }
});

//
// Tests for replacement style updates.
//

changeShardKeyOptions.forEach(function(updateConfig) {
    let runInTxn, isFindAndModify, upsert;
    [runInTxn, isFindAndModify, upsert] = [updateConfig[0], updateConfig[1], updateConfig[2]];

    jsTestLog("Testing changing the shard key using replacement style update and " +
              (isFindAndModify ? "findAndModify command " : "update command ") +
              (runInTxn ? "in transaction " : "as retryable write"));

    let session = st.s.startSession({retryWrites: runInTxn ? false : true});
    let sessionDB = session.getDatabase(kDbName);

    assertCanUpdatePrimitiveShardKey(st,
                                     kDbName,
                                     ns,
                                     session,
                                     sessionDB,
                                     runInTxn,
                                     isFindAndModify,
                                     [{"x": 300}, {"x": 4}],
                                     [{"x": 30}, {"x": 600}],
                                     upsert);
    assertCanUpdateDottedPath(st,
                              kDbName,
                              ns,
                              session,
                              sessionDB,
                              runInTxn,
                              isFindAndModify,
                              [{"x.a": 300}, {"x.a": 4}],
                              [{"x": {"a": 30}}, {"x": {"a": 600}}],
                              upsert);
    assertCanUpdatePartialShardKey(st,
                                   kDbName,
                                   ns,
                                   session,
                                   sessionDB,
                                   runInTxn,
                                   isFindAndModify,
                                   [{"x": 300, "y": 80}, {"x": 4, "y": 3}],
                                   [{"x": 30, "y": 80}, {"x": 600, "y": 3}],
                                   upsert);

    assertCanUnsetSKField(
        st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"x": 300}, {}, upsert);

    // Failure cases. These tests do not take 'upsert' as an option so we do not need to test
    // them for both upsert true and false.
    if (!upsert) {
        assertCannotUpdate_id(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"_id": 300}, {
                "_id": 30
            });
        assertCannotUpdate_idDottedPath(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"_id.a": 300}, {
                "_id": {"a": 30}
            });
        if (!isFindAndModify) {
            assertCannotUpdateWithMultiTrue(
                st, kDbName, ns, session, sessionDB, runInTxn, {"x": 300}, {"x": 30});
        }
        assertCannotUpdateSKToArray(
            st, kDbName, ns, session, sessionDB, runInTxn, isFindAndModify, {"x": 300}, {
                "x": [30]
            });
    }
});

let session = st.s.startSession({retryWrites: true});
let sessionDB = session.getDatabase(kDbName);

let docsToInsert =
    [{"x": 4, "a": 3}, {"x": 78}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];

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
assert(res.getWriteError().errmsg.includes(
    "There is either an orphan for this document or _id for this collection is not globally unique."));

session.startTransaction();
res = sessionDB.foo.update({"x": 505}, {"$set": {"x": 20}});
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert(res.errmsg.includes(
    "There is either an orphan for this document or _id for this collection is not globally unique."));
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

mongos.getDB(kDbName).foo.drop();

// ----Assert retryable write result has WCE when the internal commitTransaction fails----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
cleanupOrphanedDocs(st, ns);

// Turn on failcommand fail point to fail CoordinateCommitTransaction
assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).adminCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        writeConcernError: {code: NumberInt(12345), errmsg: "dummy error"},
        failCommands: ["coordinateCommitTransaction"],
        failInternalCommands: true
    }
}));

res = sessionDB.foo.update({x: 4}, {$set: {x: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(12345, res.getWriteConcernError().code);

let findAndModCmd = {
    findAndModify: 'foo',
    query: {x: 78},
    update: {$set: {x: 250}},
    lsid: {id: UUID()},
    txnNumber: NumberLong(1),
};
res = sessionDB.runCommand(findAndModCmd);
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(res.writeConcernError.code, 12345);
assert(res.writeConcernError.errmsg.includes("dummy error"));

assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).adminCommand({
    configureFailPoint: "failCommand",
    mode: "off",
}));

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
cleanupOrphanedDocs(st, ns);

session.startTransaction();
let id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
assert.commandWorked(sessionDB.foo.update({"x": 30}, {"$set": {"x": 600}}));
assert.commandWorked(sessionDB.foo.update({"x": 4}, {"$set": {"x": 50}}));
assert.commandWorked(session.commitTransaction_forTesting());

assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 600}).itcount());
assert.eq(id, mongos.getDB(kDbName).foo.find({"x": 600}).toArray()[0]._id);
assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 4}).itcount());
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 50}).itcount());

mongos.getDB(kDbName).foo.drop();

// Check that doing $inc on doc A, then updating shard key for doc A, then $inc again only incs
// once
shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
cleanupOrphanedDocs(st, ns);

session.startTransaction();
assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$set": {"x": 30}}));
assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
assert.commandWorked(session.commitTransaction_forTesting());

assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).itcount());
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30}).itcount());
assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).itcount());
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 30, "a": 7}).itcount());

mongos.getDB(kDbName).foo.drop();

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
cleanupOrphanedDocs(st, ns);

// Insert and $inc before moving doc
session.startTransaction();
id = mongos.getDB(kDbName).foo.find({"x": 500}).toArray()[0]._id;
assert.commandWorked(sessionDB.foo.insert({"x": 1, "a": 1}));
assert.commandWorked(sessionDB.foo.update({"x": 500}, {"$inc": {"a": 1}}));
sessionDB.foo.findAndModify({query: {"x": 500}, update: {$set: {"x": 20}}});
assert.commandWorked(session.commitTransaction_forTesting());

assert.eq(0, mongos.getDB(kDbName).foo.find({"x": 500}).toArray().length);
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 20}).toArray().length);
assert.eq(20, mongos.getDB(kDbName).foo.find({"_id": id}).toArray()[0].x);
assert.eq(1, mongos.getDB(kDbName).foo.find({"a": 7}).toArray().length);
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 20, "a": 7}).toArray().length);
assert.eq(1, mongos.getDB(kDbName).foo.find({"x": 1}).toArray().length);

mongos.getDB(kDbName).foo.drop();

// ----Assert correct behavior when update is sent directly to a shard----

shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, {"x": 100}, {"x": 300});
cleanupOrphanedDocs(st, ns);

//
// For Op-style updates.
//

// An update sent directly to a shard cannot change the shard key.
assert.commandFailedWithCode(
    st.rs1.getPrimary().getDB(kDbName).foo.update({"x": 500}, {$set: {"x": 2}}),
    ErrorCodes.ImmutableField);
assert.commandFailedWithCode(
    st.rs1.getPrimary().getDB(kDbName).foo.update({"x": 1000}, {$set: {"x": 2}}, {upsert: true}),
    ErrorCodes.ImmutableField);
assert.commandFailedWithCode(
    st.rs0.getPrimary().getDB(kDbName).foo.update({"x": 1000}, {$set: {"x": 2}}, {upsert: true}),
    ErrorCodes.ImmutableField);

// The query will not match a doc and upsert is false, so this will not fail but will be a
// no-op.
res = assert.commandWorked(
    st.rs0.getPrimary().getDB(kDbName).foo.update({"x": 500}, {$set: {"x": 2}}));
assert.eq(0, res.nMatched);
assert.eq(0, res.nModified);
assert.eq(0, res.nUpserted);

//
// For Replacement style updates.
//

// An update sent directly to a shard cannot change the shard key.
assert.commandFailedWithCode(st.rs1.getPrimary().getDB(kDbName).foo.update({"x": 500}, {"x": 2}),
                             ErrorCodes.ImmutableField);
assert.commandFailedWithCode(
    st.rs1.getPrimary().getDB(kDbName).foo.update({"x": 1000}, {"x": 2}, {upsert: true}),
    ErrorCodes.ImmutableField);
assert.commandFailedWithCode(
    st.rs0.getPrimary().getDB(kDbName).foo.update({"x": 1000}, {"x": 2}, {upsert: true}),
    ErrorCodes.ImmutableField);

// The query will not match a doc and upsert is false, so this will not fail but will be a
// no-op.
res = assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).foo.update({"x": 500}, {"x": 2}));
assert.eq(0, res.nMatched);
assert.eq(0, res.nModified);
assert.eq(0, res.nUpserted);

mongos.getDB(kDbName).foo.drop();

st.stop();
})();
