/**
 * Tests that the untrackUnshardedCollection command is consistent with the execution of movePrimary
 * commands
 * @tags: [
 *   requires_fcv_81,
 *   # Requires a deterministic placement for the collection.
 *   assumes_balancer_off,
 *   # Test cases involve the execution of movePrimary
 *   requires_2_or_more_shards,
 *   # Require control over which namespaces are/are not tracked
 *   assumes_unsharded_collection,
 *   assumes_no_track_upon_creation,
 *   # movePrimary is not an idempotent command
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

jsTest.log("Untrack a collection after performing movePrimary commands works as expected");
{
    // First change the primary for the parent DB of the untracked collection...
    assert.commandWorked(db.adminCommand({movePrimary: kDbName, to: anotherShard}));

    // then move and track the collection, placing it outside the current primary...
    assert.commandWorked(db.adminCommand({moveCollection: kNss, toShard: originalPrimaryShard}));
    assert.commandFailedWithCode(db.adminCommand({untrackUnshardedCollection: kNss}),
                                 ErrorCodes.OperationFailed);
    verifyCollectionTrackingState(
        db, kNss, true /*expectedToBeTracked*/, true /*expectedToBeUnsplittable*/);

    // ... and invoke movePrimary once again to allow untrackCollection to succeed.
    assert.commandWorked(db.adminCommand({movePrimary: kDbName, to: originalPrimaryShard}));

    assert.commandWorked(db.adminCommand({untrackUnshardedCollection: kNss}));
    verifyCollectionTrackingState(db, kNss, false /*expectedToBeTracked*/);
}
