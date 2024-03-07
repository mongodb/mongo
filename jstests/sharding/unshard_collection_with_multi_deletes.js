/**
 * Test the correctness of multi deletes during unshardCollection.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "unshardDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

assert.commandWorked(sourceCollection.insert([{x: 1}, {x: 3}, {x: 3}, {x: 1}]));

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withUnshardCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
    // be applied by the ReshardingOplogApplier.
    reshardingTest.awaitCloneTimestampChosen();

    assert.commandWorked(sourceCollection.remove({x: 1}, {justOne: false}));
});
reshardingTest.teardown();
