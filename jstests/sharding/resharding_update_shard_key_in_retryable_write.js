/**
 * Test that a retryable update or findAndModify write statement that causes a document to change
 * its shard key value during resharding is not retryable on the recipient after resharding
 * completes.
 *
 * @tags: [requires_fcv_60]
 */
(function() {

"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function runTest(reshardInPlace) {
    const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;
    const reshardingOptions = {
        primaryShardName: donorShardNames[0],
        oldShardKeyPattern: {oldShardKey: 1},
        oldShardKeyChunks: [
            {min: {oldShardKey: MinKey}, max: {oldShardKey: 0}, shard: donorShardNames[0]},
            {min: {oldShardKey: 0}, max: {oldShardKey: MaxKey}, shard: donorShardNames[1]}
        ],
        newShardKeyPattern: {newShardKey: 1},
        newShardKeyChunks: [
            {min: {newShardKey: MinKey}, max: {newShardKey: 0}, shard: recipientShardNames[0]},
            {min: {newShardKey: 0}, max: {newShardKey: MaxKey}, shard: recipientShardNames[1]}
        ],
    };

    const dbName = "testDb";
    const collName = "mongosTestColl";
    const ns = dbName + "." + collName;

    const mongosTestColl = reshardingTest.createShardedCollection({
        ns: ns,
        shardKeyPattern: reshardingOptions.oldShardKeyPattern,
        chunks: reshardingOptions.oldShardKeyChunks,
        primaryShardName: reshardingOptions.primaryShardName,
    });
    const mongosConn = mongosTestColl.getMongo();
    const mongosTestDB = mongosConn.getDB(dbName);

    // Test commands that the shard key of a document in the test collection from change its shard
    // key. Note we don't test the remove:true case because the document can't move shards if it is
    // being deleted.
    const updateCmdObj = {
        update: collName,
        updates: [
            {q: {oldShardKey: -1, newShardKey: -1}, u: {$set: {oldShardKey: 1}}},
        ],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(1),
    };

    const findAndModifyUpdateCmdObj = {
        findAndModify: collName,
        query: {oldShardKey: -2, newShardKey: -2},
        update: {$set: {oldShardKey: 2}},
        lsid: {id: UUID()},
        txnNumber: NumberLong(2),
    };

    const findAndModifyUpsertCmdObj = {
        findAndModify: collName,
        query: {oldShardKey: -3, newShardKey: -3},
        update: {$set: {oldShardKey: 3}},
        upsert: true,
        lsid: {id: UUID()},
        txnNumber: NumberLong(3),
    };

    const expectedTransientErrors = new Set([
        ErrorCodes.StaleConfig,
        ErrorCodes.NoSuchTransaction,
        ErrorCodes.ShardCannotRefreshDueToLocksHeld,
        ErrorCodes.LockTimeout,
        ErrorCodes.IncompleteTransactionHistory
    ]);

    function runCommandRetryOnTransientErrors(db, cmdObj) {
        let res;
        assert.soon(() => {
            res = db.runCommand(cmdObj);

            if (expectedTransientErrors.has(res.code) ||
                (res.writeErrors && expectedTransientErrors.has(res.writeErrors[0].code))) {
                cmdObj.txnNumber = NumberLong(cmdObj.txnNumber + 1);
                return false;
            }
            assert.commandWorked(res);
            return true;
        });
        return res;
    }

    assert.commandWorked(mongosTestColl.insert({oldShardKey: -1, newShardKey: -1}));
    assert.commandWorked(mongosTestColl.insert({oldShardKey: -2, newShardKey: -2}));

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: reshardingOptions.newShardKeyPattern,
            newChunks: reshardingOptions.newShardKeyChunks,
        },
        () => {
            // The cloneTimestamp is the boundary for whether a retryable write statement will
            // be retryable after the resharding operation completes.
            assert.soon(() => {
                const coordinatorDoc =
                    mongosConn.getCollection("config.reshardingOperations").findOne({ns: ns});

                return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
            });

            jsTest.log("Start running retryable writes during resharding");

            const updateRes = runCommandRetryOnTransientErrors(mongosTestDB, updateCmdObj);
            assert.eq(updateRes.n, 1, tojson(updateRes));
            assert.eq(updateRes.nModified, 1, tojson(updateRes));

            const findAndModifyUpdateRes =
                runCommandRetryOnTransientErrors(mongosTestDB, findAndModifyUpdateCmdObj);
            assert.eq(findAndModifyUpdateRes.lastErrorObject.n, 1, tojson(findAndModifyUpdateRes));
            assert.eq(findAndModifyUpdateRes.lastErrorObject.updatedExisting,
                      true,
                      tojson(findAndModifyUpdateRes));

            let findAndModifyUpsertRes =
                runCommandRetryOnTransientErrors(mongosTestDB, findAndModifyUpsertCmdObj);
            assert.eq(findAndModifyUpsertRes.lastErrorObject.n, 1, tojson(findAndModifyUpsertRes));
            assert.eq(findAndModifyUpsertRes.lastErrorObject.updatedExisting,
                      false,
                      tojson(findAndModifyUpsertRes));
            assert(findAndModifyUpsertRes.lastErrorObject.upserted, tojson(findAndModifyUpsertRes));

            jsTest.log("Finished running retryable writes during resharding");
        });

    assert.eq(mongosTestColl.find({oldShardKey: -1, newShardKey: -1}).itcount(), 0);
    assert.eq(mongosTestColl.find({oldShardKey: -2, newShardKey: -2}).itcount(), 0);
    assert.eq(mongosTestColl.find({oldShardKey: 1, newShardKey: -1}).itcount(), 1);
    assert.eq(mongosTestColl.find({oldShardKey: 2, newShardKey: -2}).itcount(), 1);
    assert.eq(mongosTestColl.find({oldShardKey: 3, newShardKey: -3}).itcount(), 1);

    jsTest.log("Start retrying retryable writes after resharding");
    assert.commandFailedWithCode(mongosTestDB.runCommand(updateCmdObj),
                                 ErrorCodes.IncompleteTransactionHistory);
    assert.commandFailedWithCode(mongosTestDB.runCommand(findAndModifyUpdateCmdObj),
                                 ErrorCodes.IncompleteTransactionHistory);
    assert.commandFailedWithCode(mongosTestDB.runCommand(findAndModifyUpsertCmdObj),
                                 ErrorCodes.IncompleteTransactionHistory);
    jsTest.log("Finished retrying retryable writes after resharding");

    assert.eq(mongosTestColl.find({oldShardKey: -1, newShardKey: -1}).itcount(), 0);
    assert.eq(mongosTestColl.find({oldShardKey: -2, newShardKey: -2}).itcount(), 0);
    assert.eq(mongosTestColl.find({oldShardKey: 1, newShardKey: -1}).itcount(), 1);
    assert.eq(mongosTestColl.find({oldShardKey: 2, newShardKey: -2}).itcount(), 1);
    assert.eq(mongosTestColl.find({oldShardKey: 3, newShardKey: -3}).itcount(), 1);

    reshardingTest.teardown();
}

runTest(true /* reshardInPlace */);
runTest(false /* reshardInPlace */);
})();
