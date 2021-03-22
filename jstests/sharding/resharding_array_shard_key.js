/**
 * Tests that during resharding, inserts and updates that specify an array for the new shard key
 * fail.
 *
 * @tags: [requires_fcv_49, uses_atclustertime]
 */

(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;

const whileReshardingCollection = reshardingTest.createShardedCollection({
    ns: "test.whileResharding",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]},
    ],
});

assert.commandWorked(whileReshardingCollection.insert({_id: 0, oldKey: -20, newKey: 20}));

const recipientShardNames = reshardingTest.recipientShardNames;

function awaitEstablishmentOfCloneTimestamp(inputCollection) {
    const mongos = inputCollection.getMongo();
    assert.soon(() => {
        const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
            ns: inputCollection.getFullName()
        });
        return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
    });
}

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        awaitEstablishmentOfCloneTimestamp(whileReshardingCollection);

        const testDB = whileReshardingCollection.getDB();
        let session = testDB.getMongo().startSession({retryWrites: true});
        let sessionDB = session.getDatabase("test");

        // Once the resharding operation begins, the donor shard will require that the shard key
        // value under both the current and new key patterns is valid.
        assert.commandFailedWithCode(
            sessionDB.whileResharding.update({_id: 0, oldKey: -20}, {$set: {newKey: [1, 2]}}),
            ErrorCodes.ShardKeyNotFound);

        assert.commandFailedWithCode(
            sessionDB.whileResharding.insert({_id: 1, oldKey: -11, newKey: [1, 2]}),
            ErrorCodes.ShardKeyNotFound);
    });

const insertCollection = reshardingTest.createShardedCollection({
    ns: "test.insertTest",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]},
    ],
});

assert.commandWorked(insertCollection.insert({_id: 0, oldKey: -10, newKey: [1, 2]}));

reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
},
                                          (tempNs) => {},
                                          {expectedErrorCode: ErrorCodes.ShardKeyNotFound});

reshardingTest.teardown();
})();
