/**
 * Test the correctness of multi deletes during unsplittable moveCollection.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *  multiversion_incompatible,
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createUnshardedCollection(
    {ns: "reshardingDb.coll", primaryShardName: donorShardNames[0]});

assert.commandWorked(sourceCollection.insert([{x: 1}, {x: 3}, {x: 3}, {x: 1}]));

const recipientShardNames = reshardingTest.recipientShardNames;

reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
    // be applied by the ReshardingOplogApplier.
    reshardingTest.awaitCloneTimestampChosen();

    assert.commandWorked(sourceCollection.remove({x: 1}, {justOne: false}));
});

reshardingTest.teardown();
