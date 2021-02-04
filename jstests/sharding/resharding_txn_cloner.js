/**
 * Tests the resharding recipient shards handles config.transactions entries from the source
 * shards.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 3, numRecipients: 3, reshardInPlace: true});

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: 100}, shard: donorShardNames[1]},
        {min: {oldKey: 100}, max: {oldKey: MaxKey}, shard: donorShardNames[2]},
    ],
});

let lsidList = [];
lsidList.push(UUID());
lsidList.push(UUID());
lsidList.push(UUID());

let execRetryableInsert = function(lsid, doc) {
    return inputCollection.getDB('reshardingDb').runCommand({
        insert: 'coll',
        documents: [doc],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(1),
    });
};

assert.commandWorked(execRetryableInsert(lsidList[0], {oldKey: -10, newKey: 0}));
assert.commandWorked(execRetryableInsert(lsidList[1], {oldKey: 0, newKey: 100}));
assert.commandWorked(execRetryableInsert(lsidList[2], {oldKey: 100, newKey: -10}));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[1]},
        {min: {newKey: 0}, max: {newKey: 100}, shard: recipientShardNames[2]},
        {min: {newKey: 100}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
    ],
});

// If we don't refresh mongos, writes will be targetted based on the chunk distribution before
// resharding. Even though the shard versions don't match, it will not cause a stale config
// exception because the write was already executed on the shard being targetted, resulting in a
// no-op write, and thus, no shard version checking. This behavior is not wrong, but since we
// want to test the retry behavior after resharding, we force the mongos to refresh.
const mongos = inputCollection.getMongo();
assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));

assert.commandFailedWithCode(execRetryableInsert(lsidList[0], {oldKey: -10, newKey: 0}),
                             ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(execRetryableInsert(lsidList[1], {oldKey: 0, newKey: 100}),
                             ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(execRetryableInsert(lsidList[2], {oldKey: 100, newKey: -10}),
                             ErrorCodes.IncompleteTransactionHistory);

reshardingTest.teardown();
})();
