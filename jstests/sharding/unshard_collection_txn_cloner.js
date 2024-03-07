/**
 * Tests the unshardCollection recipient shard handles config.transactions entries from the source
 * shards.
 *
 * @tags: [
 *  uses_atclustertime,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 3, numRecipients: 1, reshardInPlace: true});

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
reshardingTest.withUnshardCollectionInBackground({
    toShard: recipientShardNames[0],
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

// Since we are doing a reshardInPlace operation, the recipient is one of the donors and
// this insert is targetted to the same donor, which has already executed the write and
// thus won't error.
assert.commandWorked(execRetryableInsert(lsidList[2], {oldKey: 100, newKey: -10}));

reshardingTest.teardown();
