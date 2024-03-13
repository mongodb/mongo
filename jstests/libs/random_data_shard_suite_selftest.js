/*
 * Tests that the failpoint createUnshardedCollectionRandomizeDataShard works as expected.
 *
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDb = db.getSiblingDB('random_data_shard_suite_selftest_' + Random.srand());

assert(FixtureHelpers.isMongos(db), "Must run mongos");
assert(FixtureHelpers.getAllReplicas(db).length > 1, "Needs more than one shard");

let coll = db.foo;

function checkOwningShardRandomlySelected(fn) {
    coll.drop();

    // Run the operation 'fn' one first time and annotate which owning shard was selected.
    fn();
    const initialDataShards = FixtureHelpers.getShardsOwningDataForCollection(coll);
    assert.eq(1, initialDataShards.length);

    // Drop the collection and run 'fn' again until a different owning shard is randomly selected.
    assert.soon(() => {
        assert(coll.drop());
        fn();
        const newDataShards = FixtureHelpers.getShardsOwningDataForCollection(coll);
        assert.eq(1, newDataShards.length);
        return newDataShards[0] !== initialDataShards[0];
    });

    assert(coll.drop());
}

// Check that operations that create collections (possibly implicitly) do so on a randomly selected
// shard.
checkOwningShardRandomlySelected(() => {
    assert.commandWorked(db.createCollection(coll.getName()));
});

checkOwningShardRandomlySelected(() => {
    assert.commandWorked(coll.insert({x: 1}));
});

checkOwningShardRandomlySelected(() => {
    assert.commandWorked(coll.updateOne({x: 1}, {$set: {x: 1}}, {upsert: true}));
});

checkOwningShardRandomlySelected(() => {
    assert.commandWorked(coll.createIndex({x: 1}));
});
