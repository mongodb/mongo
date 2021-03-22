/**
 * Verify that the cloning phase of a resharding operation takes at least
 * reshardingMinimumOperationDurationMillis to complete. This will indirectly verify that the
 * txnCloners were not started until after waiting for reshardingMinimumOperationDurationMillis to
 * elapse.
 *
 * @tags: [requires_fcv_49, uses_atclustertime]
 */

(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const minimumOperationDurationMS = 30000;

const reshardingTest = new ReshardingTest({
    numDonors: 2,
    numRecipients: 2,
    reshardInPlace: true,
    minimumOperationDurationMS: minimumOperationDurationMS
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

assert.commandWorked(sourceCollection.insert([
    {_id: "stays on shard0", oldKey: -10, newKey: -10, counter: 0},
    {_id: "moves to shard0", oldKey: 10, newKey: -10, counter: 0},
]));

const mongos = sourceCollection.getMongo();
const session = mongos.startSession({causalConsistency: false, retryWrites: false});
const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                              .getCollection(sourceCollection.getName());

function runRetryableWrite(phase, expectedErrorCode = ErrorCodes.OK) {
    const res = sessionCollection.runCommand("update", {
        updates: [
            {q: {_id: "stays on shard0"}, u: {$inc: {counter: 1}}},
            {q: {_id: "moves to shard0"}, u: {$inc: {counter: 1}}},
        ],
        txnNumber: NumberLong(1)
    });

    if (expectedErrorCode === ErrorCodes.OK) {
        assert.commandWorked(res);
    } else {
        assert.commandFailedWithCode(res, expectedErrorCode);
    }

    const docs = sourceCollection.find().toArray();
    assert.eq(2, docs.length, {docs});

    for (const doc of docs) {
        assert.eq(1,
                  doc.counter,
                  {message: `retryable write executed more than once ${phase}`, id: doc._id, docs});
    }
}

runRetryableWrite("before resharding");

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        runRetryableWrite("during resharding");

        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        runRetryableWrite("during resharding after cloneTimestamp was chosen");

        let startTime = Date.now();

        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.state === "applying";
        });

        const epsilon = 5000;
        const elapsed = Date.now() - startTime;
        assert.gt(elapsed, minimumOperationDurationMS - epsilon);

        runRetryableWrite("during resharding after collection cloning had finished",
                          ErrorCodes.IncompleteTransactionHistory);
    });

runRetryableWrite("after resharding", ErrorCodes.IncompleteTransactionHistory);

reshardingTest.teardown();
})();