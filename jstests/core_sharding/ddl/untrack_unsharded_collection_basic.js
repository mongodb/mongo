/**
 * Basic test for the untrackUnshardedCollection command.
 * @tags: [
 *   requires_fcv_81,
 *   # Requires a deterministic placement for the collection.
 *   assumes_balancer_off,
 *   # Test cases involve the execution of movePrimary
 *   requires_2_or_more_shards,
 *   # Require control over which namespaces are/are not tracked
 *   assumes_unsharded_collection,
 *   assumes_no_track_upon_creation,
 *   # untrackCollection requires a custom value for the minSnapshotHistoryWindowInSeconds server
 *   # parameter, which is reset when a node is killed.
 *   does_not_support_stepdowns,
 * ]
 */

import {
    getRandomShardName,
    verifyCollectionTrackingState
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

// Setup an untracked collection
db.dropDatabase();

const kDbName = db.getName();
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;

assert.commandWorked(db[kCollName].insert({x: 1}));

const originalPrimaryShard = db.getDatabasePrimaryShardId();
const anotherShard = getRandomShardName(db, [originalPrimaryShard]);

jsTest.log("Untrack a non tracked collection is a noop.");
{
    assert.commandWorked(db.adminCommand({untrackUnshardedCollection: kNss}));
    verifyCollectionTrackingState(db, kNss, false /*expectedToBeTracked*/);
}

jsTest.log("Untrack a collection placed outside its non-primary shard returns error.");
{
    // Move the collection outside its primary shard; this will also make it tracked.
    assert.commandWorked(db.adminCommand({moveCollection: kNss, toShard: anotherShard}));
    verifyCollectionTrackingState(
        db, kNss, true /*expectedToBeTracked*/, true /*expectedToBeUnsplittable*/);

    assert.commandFailedWithCode(db.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    verifyCollectionTrackingState(
        db, kNss, true /*expectedToBeTracked*/, true /*expectedToBeUnsplittable*/);
}

jsTest.log("Untrack a collection placed on its primary shard succeeds.");
{
    assert.commandWorked(db.adminCommand({moveCollection: kNss, toShard: originalPrimaryShard}));
    assert.commandWorked(db.adminCommand({untrackUnshardedCollection: kNss}));
    verifyCollectionTrackingState(db, kNss, false /*expectedToBeTracked*/);
}
