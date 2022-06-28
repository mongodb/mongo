/**
 * When we update a shard key such that a document moves from one shard to another, and the
 * update is sent as a retryable write, the update is converted into a multi-statement
 * transaction with the same transaction number. The new transaction contains a delete on the source
 * shard and an insert on the destination shard. If original update is retried, it should receive an
 * error saying that the write can't be retried since it was upgraded to a transaction as part of
 * the update. This should be true whether or not a migration occurs on the chunk containing the
 * original value of the document's shard key. This file tests that behavior.
 * @tags: [uses_transactions, uses_multi_shard_transaction,]
 */
(function() {

"use strict";

load('jstests/sharding/libs/sharded_transactions_helpers.js');
load('./jstests/libs/chunk_manipulation_util.js');

// For startParallelOps to write its state
let staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({
    mongos: 2,
    shards: 3,
    rs: {nodes: 2},
    rsOptions:
        {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}}
});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
let mongos0TestDB = st.s0.getDB(dbName);
let mongos0TestColl = mongos0TestDB.getCollection(collName);
let mongos1TestDB = st.s1.getDB(dbName);

// Create a sharded collection with three chunks:
//     [-inf, -10), [-10, 10), [10, inf)
assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: -10}}));
assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 10}}));

/**
 * Sets up a test by moving chunks to such that one chunk is on each
 * shard, with the following distribution:
 *     shard0: [-inf, -10)
 *     shard1: [-10, 10)
 *     shard2: [10, inf)
 */
function setUp() {
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: ns, find: {x: -100}, to: st.shard0.shardName}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s0.adminCommand({moveChunk: ns, find: {x: 1000}, to: st.shard2.shardName}));

    flushRoutersAndRefreshShardMetadata(st, {ns});
}

/**
 * Tears down a test by dropping all documents from the test collection.
 */
function tearDown() {
    assert.commandWorked(mongos0TestColl.deleteMany({}));
}

/**
 * Generic function to run a test. 'description' is a description of the test for logging
 * purposes and 'testBody' is the test function.
 */
function test(description, testBody) {
    jsTest.log(`Running Test Setup: ${description}`);
    setUp();
    jsTest.log(`Running Test Body: ${description}`);
    testBody();
    jsTest.log(`Running Test Tear-Down: ${description}`);
    tearDown();
    jsTest.log(`Finished Running Test: ${description}`);
}

const shardKeyValueOnShard0 = -100;
const shardKeyValueOnShard1 = 0;

// Test commands that change the shard key of a document in the test collection from
// shardKeyValueOnShard0 to shardKeyValueOnShard1. Note we don't test the remove:true case
// because the document can't move shards if it is being deleted.
const updateCmdObjBase = {
    update: collName,
    updates: [
        {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
    ],
    ordered: false,
};

const findAndModifyUpdateCmdObjBase = {
    findAndModify: collName,
    query: {x: shardKeyValueOnShard0},
    update: {$set: {x: shardKeyValueOnShard1}},
};

const findAndModifyUpsertCmdObjBase = {
    findAndModify: collName,
    query: {x: shardKeyValueOnShard0},
    update: {$set: {x: shardKeyValueOnShard1}},
    upsert: true,
};

function attachTxnFields(cmdObj) {
    const cmdObjWithTxnFields = Object.assign({}, cmdObj);
    cmdObjWithTxnFields.lsid = {id: UUID()};
    cmdObjWithTxnFields.txnNumber = NumberLong(35);
    return cmdObjWithTxnFields;
}

{
    // Run the given command and assert the response is as expected.
    function runCommandOnInitialTry(cmdObj) {
        // Insert a single document on shard0. Skip in the upsert case to get coverage where there
        // is no pre-image.
        if (!cmdObj.upsert) {
            mongos0TestColl.insert({x: shardKeyValueOnShard0});
        }

        // Update the document shard key. The document should now be on shard1.
        const result = assert.commandWorked(mongos0TestDB.runCommand(cmdObj));
        if (cmdObj.findAndModify) {
            if (!cmdObj.upsert) {
                assert.eq(result.lastErrorObject.n, 1, tojson(result));
                assert.eq(result.lastErrorObject.updatedExisting, true, tojson(result));
            } else {
                assert.eq(result.lastErrorObject.n, 1, tojson(result));
                assert.eq(result.lastErrorObject.updatedExisting, false, tojson(result));
                assert(result.lastErrorObject.upserted, tojson(result));
            }
        } else {  // update
            assert.eq(result.n, 1, tojson(result));
            assert.eq(result.nModified, 1, tojson(result));
        }
        assert.eq(mongos0TestColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

        assert.commandWorked(mongos0TestColl.deleteMany({}));  // Clean up for the next test case.
    }

    test("Updating shard key in retryable write receives error on retry", () => {
        const updateCmdObj = attachTxnFields(updateCmdObjBase);
        const findAndModifyUpdateCmdObj = attachTxnFields(findAndModifyUpdateCmdObjBase);
        const findAndModifyUpsertCmdObj = attachTxnFields(findAndModifyUpsertCmdObjBase);

        runCommandOnInitialTry(updateCmdObj);
        runCommandOnInitialTry(findAndModifyUpdateCmdObj);
        runCommandOnInitialTry(findAndModifyUpsertCmdObj);

        // Retry the commands. They should run against shard0, which should throw
        // IncompleteTransactionHistory.
        assert.commandFailedWithCode(mongos0TestDB.runCommand(updateCmdObj),
                                     ErrorCodes.IncompleteTransactionHistory);
        assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpdateCmdObj),
                                     ErrorCodes.IncompleteTransactionHistory);
        assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                     ErrorCodes.IncompleteTransactionHistory);
    });

    test("Updating shard key in retryable write receives error on retry when the original chunk " +
             "has been migrated to a new shard",
         () => {
             const updateCmdObj = attachTxnFields(updateCmdObjBase);
             const findAndModifyUpdateCmdObj = attachTxnFields(findAndModifyUpdateCmdObjBase);
             const findAndModifyUpsertCmdObj = attachTxnFields(findAndModifyUpsertCmdObjBase);

             runCommandOnInitialTry(updateCmdObj);
             runCommandOnInitialTry(findAndModifyUpdateCmdObj);
             runCommandOnInitialTry(findAndModifyUpsertCmdObj);

             // Move the chunk that contained the original document to shard1.
             assert.commandWorked(st.s0.adminCommand(
                 {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard1.shardName}));

             // Retry the command. This should retry against shard1, which should throw
             // IncompleteTransactionHistory.
             assert.commandFailedWithCode(mongos0TestDB.runCommand(updateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpdateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
         });

    test("Updating shard key in retryable write receives error on retry when the original chunk " +
             "has been migrated to a new shard and then to a third shard",
         () => {
             const updateCmdObj = attachTxnFields(updateCmdObjBase);
             const findAndModifyUpdateCmdObj = attachTxnFields(findAndModifyUpdateCmdObjBase);
             const findAndModifyUpsertCmdObj = attachTxnFields(findAndModifyUpsertCmdObjBase);

             runCommandOnInitialTry(updateCmdObj);
             runCommandOnInitialTry(findAndModifyUpdateCmdObj);
             runCommandOnInitialTry(findAndModifyUpsertCmdObj);

             // Move the chunk that contained the original document to shard1.
             assert.commandWorked(st.s0.adminCommand(
                 {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard1.shardName}));

             // Then move the same chunk that contained the original document to shard2.
             assert.commandWorked(st.s0.adminCommand(
                 {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard2.shardName}));

             // Retry the command. This should retry against shard1, which should throw
             // IncompleteTransactionHistory.
             assert.commandFailedWithCode(mongos0TestDB.runCommand(updateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpdateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
         });

    test("Updating shard key in retryable write receives error on retry when the original chunk " +
             "has been migrated to a shard without knowledge of the transaction",
         () => {
             const updateCmdObj = attachTxnFields(updateCmdObjBase);
             const findAndModifyUpdateCmdObj = attachTxnFields(findAndModifyUpdateCmdObjBase);
             const findAndModifyUpsertCmdObj = attachTxnFields(findAndModifyUpsertCmdObjBase);

             runCommandOnInitialTry(updateCmdObj);
             runCommandOnInitialTry(findAndModifyUpdateCmdObj);
             runCommandOnInitialTry(findAndModifyUpsertCmdObj);

             // Move the chunk that contained the original document to shard2, which does not know
             // about the transaction.
             assert.commandWorked(st.s0.adminCommand(
                 {moveChunk: ns, find: {x: shardKeyValueOnShard0}, to: st.shard2.shardName}));

             // Retry the command. This should retry against shard2, which should throw
             // IncompleteTransactionHistory.
             assert.commandFailedWithCode(mongos0TestDB.runCommand(updateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpdateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
         });
}

test(
    "config.transactions entries for single-shard transactions which commit during transferMods phase are successfully migrated as dead-end sentinels",
    () => {
        const shardKeyValueOnShard0 = -100;
        const lsid = {id: UUID()};
        const txnNumber = 35;

        // Insert a single document on shard0.
        assert.commandWorked(mongos0TestColl.insert({x: shardKeyValueOnShard0}));

        const cmdToRunInTransaction = {
            update: collName,
            updates: [
                // Add a new field.
                {q: {x: shardKeyValueOnShard0}, u: {$set: {a: 4}}},
            ],
            ordered: false,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false
        };

        const fakeRetryCmd = {
            update: collName,
            updates: [
                // Add a new field.
                {q: {x: shardKeyValueOnShard0}, u: {$set: {a: 4}}},
            ],
            ordered: false,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber)
        };

        pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        let joinMoveChunk = moveChunkParallel(
            staticMongod, st.s0.host, {x: shardKeyValueOnShard0}, null, ns, st.shard1.shardName);

        waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        // Update a document being migrated.
        const result = assert.commandWorked(mongos0TestDB.runCommand(cmdToRunInTransaction));
        assert.eq(result.n, 1);
        assert.eq(result.nModified, 1);

        assert.commandWorked(mongos0TestDB.adminCommand({
            commitTransaction: 1,
            writeConcern: {w: "majority"},
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false
        }));

        // Check that the update from the transaction succeeded.
        const resultingDoc = mongos0TestColl.findOne({x: shardKeyValueOnShard0});
        assert.neq(resultingDoc, null);
        assert.eq(resultingDoc["a"], 4);

        unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        // Wait for moveChunk to complete
        joinMoveChunk();

        st.printShardingStatus();
        // Retry the command. This should retry against shard1, which should throw
        // IncompleteTransactionHistory.
        assert.commandFailedWithCode(mongos0TestDB.runCommand(fakeRetryCmd),
                                     ErrorCodes.IncompleteTransactionHistory);
    });

{
    // Run the given command as during the transferMods phase of chunk migration is migrated
    // successfully to shard2, which is not involved in the shard key update.
    function runCommandDuringChunkMigration(cmdObj) {
        const docId = 0;

        // Insert a single document on shard0. Skip in the upsert case to get coverage where there
        // is no pre-image.
        if (!cmdObj.upsert) {
            mongos0TestColl.insert({_id: docId, x: shardKeyValueOnShard0});
        }

        pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        // We're going to do a shard key update to move a document from shard0 to shard1, so
        // here we move the chunk from shard0 to shard2, which won't be involved in the
        // transaction created by the shard key update.
        let joinMoveChunk = moveChunkParallel(
            staticMongod, st.s0.host, {x: shardKeyValueOnShard0}, null, ns, st.shard2.shardName);

        waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        // Update the document shard key so that the document will move from shard0 to shard1.
        const result = assert.commandWorked(mongos0TestDB.runCommand(cmdObj));
        if (cmdObj.findAndModify) {
            if (!cmdObj.upsert) {
                assert.eq(result.lastErrorObject.n, 1, tojson(result));
                assert.eq(result.lastErrorObject.updatedExisting, true, tojson(result));
            } else {
                assert.eq(result.lastErrorObject.n, 1, tojson(result));
                assert.eq(result.lastErrorObject.updatedExisting, false, tojson(result));
                assert(result.lastErrorObject.upserted, tojson(result));
            }
        } else {  // update
            assert.eq(result.n, 1, tojson(result));
            assert.eq(result.nModified, 1, tojson(result));
        }

        unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

        // Wait for moveChunk to complete
        joinMoveChunk();

        st.printShardingStatus();
    }

    test("Update shard key in retryable write during transferMods phase of chunk migration " +
             "is migrated successfully to a node not involved in the shard key update",
         () => {
             const updateCmdObj = attachTxnFields(updateCmdObjBase);
             runCommandDuringChunkMigration(updateCmdObj);
             // Retry the command. This should retry against shard2, which should throw
             // IncompleteTransactionHistory.
             assert.commandFailedWithCode(mongos0TestDB.runCommand(updateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
         });

    test("findAndModify update shard key in retryable write during transferMods phase of chunk " +
             "migration is migrated successfully to a node not involved in the shard key update",
         () => {
             const findAndModifyUpdateCmdObj = attachTxnFields(findAndModifyUpdateCmdObjBase);
             runCommandDuringChunkMigration(findAndModifyUpdateCmdObj);
             // Retry the command. This should retry against shard2, which should throw
             // IncompleteTransactionHistory.
             assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpdateCmdObj),
                                          ErrorCodes.IncompleteTransactionHistory);
         });

    test("findAndModify update shard key with upsert=true in retryable write during " +
             "transferMods phase of chunk migration is migrated successfully to a node not " +
             "involved in the shard key update",
         () => {
             const findAndModifyUpsertCmdObj = attachTxnFields(findAndModifyUpsertCmdObjBase);
             runCommandDuringChunkMigration(findAndModifyUpsertCmdObj);
             // Retry the command. This should retry against shard2.
             if (isUpdateDocumentShardKeyUsingTransactionApiEnabled(st.s0)) {
                 // If internal transactions are enabled, shard2 is expected to throw
                 // IncompleteTransactionHistory since it should have the WouldChangeOwningShard
                 // noop oplog entry copied from shard0 during the migration.
                 assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                              ErrorCodes.IncompleteTransactionHistory);
             } else {
                 // If internal transactions are not enabled, shard2 is expected not to throw an
                 // error since this is an upsert so on shard0 the applyOps oplog entry would not
                 // have any operations in it and therefore shard2 would not receive a dead-end
                 // sentinel oplog entry for this transaction.

                 // If the retry is done against the original mongos, it should fail with
                 // ConflictingOperationInProgress on the mongos itself when the mongos tries to
                 // convert the retryable write into a transaction again.
                 assert.commandFailedWithCode(mongos0TestDB.runCommand(findAndModifyUpsertCmdObj),
                                              ErrorCodes.ConflictingOperationInProgress);
                 // If the retry is done against a different mongos, it should fail on shard1
                 // when the shard receives a transaction statement with a txnNumber that is already
                 // committed.
                 assert.commandFailedWithCode(mongos1TestDB.runCommand(findAndModifyUpsertCmdObj),
                                              50911);
             }
         });
}

// TODO (SERVER-40815) This test currently fails with DuplicateKeyError on _id.
//
// test(
//    "Update to shard key in retryable write during transfer mods phase of chunk migration is
//    migrated successfully ",
//    () => {
//        const shardKeyValueOnShard0 = -100;
//        const shardKeyValueOnShard1 = 0;
//        const docId = 0;

//        // Insert a single document on shard 0.
//        assert.commandWorked(mongos0TestColl.insert({_id: docId, x: shardKeyValueOnShard0}));

//        const cmdObj = {
//            update: collName,
//            updates: [
//                {q: {x: shardKeyValueOnShard0}, u: {$set: {x: shardKeyValueOnShard1}}},
//            ],
//            ordered: false,
//            lsid: {id: UUID()},
//            txnNumber: NumberLong(35),
//        };

//        pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

//        let joinMoveChunk = moveChunkParallel(
//            staticMongod, st.s0.host, {x: shardKeyValueOnShard0}, null, ns,
//            st.shard1.shardName);

//        waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

//        // Update the document shard key.

//        // THIS CURRENTLY FAILS WITH DuplicateKeyError on _id
//        const result = assert.commandWorked(mongos0TestDB.runCommand(cmdObj));
//        assert.eq(result.n, 1);
//        assert.eq(result.nModified, 1);
//        assert.eq(mongos0TestColl.find({x: shardKeyValueOnShard1}).itcount(), 1);

//        unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

//        // Wait for moveChunk to complete
//        joinMoveChunk();

//        st.printShardingStatus();
//        // Retry the command. This should retry against shard 1, which should throw
//        // IncompleteTransactionHistory.
//        assert.commandFailedWithCode(mongos0TestDB.runCommand(cmdObj),
//                                     ErrorCodes.IncompleteTransactionHistory);
//    });

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
