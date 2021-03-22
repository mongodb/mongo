/**
 * Test to make sure that if there are uncommitted transactions right when the resharding command
 * begins, it will select a cloneTimestamp that is greater than the operation time of those
 * transactions when they commit.
 *
 * @tags: [requires_fcv_49, uses_atclustertime]
 */
(function() {
"use strict";
load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 10}, shard: donorShardNames[0]},
        {min: {oldKey: 10}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const mongos = sourceCollection.getMongo();
const session = mongos.startSession({causalConsistency: false, retryWrites: false});
const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                              .getCollection(sourceCollection.getName());

assert.commandWorked(sessionCollection.insert({_id: 0, oldKey: 5, newKey: 15, counter: 0}));

session.startTransaction();

assert.commandWorked(sessionCollection.update({_id: 0, oldKey: 5}, {$inc: {counter: 1}}));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 10}, shard: recipientShardNames[0]},
            {min: {newKey: 10}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        // Make sure that the resharding donor service will be blocked waiting for the MODE_S lock
        // for the collection being resharded because of a transaction that started prior to the
        // resharding command. Once we confirmed this, we can let the transaction commit and allow
        // the resharding command to complete.
        assert.soon(() => {
            let curOpOpt = {idleSessions: false, allUsers: true, idleConnections: true};
            let matchStage = {$match: {'locks.Collection': 'R', desc: /ReshardingDonorService/}};
            let curOpResults =
                mongos.getDB('admin').aggregate([{$currentOp: curOpOpt}, matchStage]).toArray();

            return curOpResults.length > 0;
        });

        let coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
            ns: sourceCollection.getFullName()
        });

        assert.neq(null, coordinatorDoc);
        assert.eq(undefined, coordinatorDoc.cloneTimestamp);

        let res = assert.commandWorked(session.commitTransaction_forTesting());
        let commitOperationTS = res.operationTime;

        assert.soon(() => {
            coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        assert.eq(1,
                  timestampCmp(coordinatorDoc.cloneTimestamp, commitOperationTS),
                  'coordinatorDoc: ' + tojson(coordinatorDoc) +
                      ', commit opTs: ' + tojson(commitOperationTS));
    });

reshardingTest.teardown();
})();
