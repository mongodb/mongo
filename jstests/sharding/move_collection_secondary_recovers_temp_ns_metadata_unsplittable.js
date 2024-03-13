/**
 * Tests that moveCollection succeeds even its oplog application runs on a newly elected primary of
 * a recipient shard.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *   # TODO (SERVER-87812) Remove multiversion_incompatible tag
 *   multiversion_incompatible,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({enableElections: true});

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createUnshardedCollection({
    ns: "reshardingDb.coll",
    primaryShardName: donorShardNames[0],
});

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
    // be applied by the ReshardingOplogApplier.
    reshardingTest.awaitCloneTimestampChosen();

    // A secondary member of the recipient shard isn't guaranteed to know the collection
    // metadata for the temporary resharding collection. We step one up to become the new
    // primary to test that resharding succeeds even when the collection metadata must be
    // recovered from the config server.
    reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
    assert.commandWorked(sourceCollection.insert({_id: 0}));

    reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
    assert.commandWorked(sourceCollection.update({_id: 0}, {$set: {extra: 3}}));

    reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
    assert.commandWorked(sourceCollection.remove({_id: 0}, {justOne: true}));
});

reshardingTest.teardown();
