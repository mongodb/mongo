/**
 * Tests that moveCollection succeeds when a participant experiences a failover or clean/unclean
 * restart during the operation.
 * Multiversion testing does not support tests that kill and restart nodes. So we had to add the
 * 'multiversion_incompatible' tag.
 * @tags: [
 *   uses_atclustertime,
 *   multiversion_incompatible,
 *   requires_persistence,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   multiversion_incompatible,
 * ]
 */
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollectionNs = "reshardingDb.coll";

const sourceCollection = reshardingTest.createUnshardedCollection(
    {ns: sourceCollectionNs, primaryShardName: donorShardNames[0]});

reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    reshardingTest.stepUpNewPrimaryOnShard(donorShardNames[0]);

    reshardingTest.killAndRestartPrimaryOnShard(recipientShardNames[0]);

    reshardingTest.shutdownAndRestartPrimaryOnShard(recipientShardNames[0]);
});

// Should have unsplittable set to true
let configDb = sourceCollection.getMongo().getDB('config');
let unshardedColl = configDb.collections.findOne({_id: sourceCollectionNs});
assert.eq(unshardedColl.unsplittable, true);
let unshardedChunk = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
assert.eq(1, unshardedChunk.length);

reshardingTest.teardown();
