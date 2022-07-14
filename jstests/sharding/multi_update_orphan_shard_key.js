/**
 * Tests it isn't possible to update an orphan document's shard key. Only multi=true updates skip
 * shard versioning. They are therefore the only case which skips ownership filtering.
 *
 *  @tags: [
 *    requires_fcv_52
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 1}});
const dbName = "test";
const collName = "update_orphan_shard_key";
const collection = st.s.getDB(dbName).getCollection(collName);

// Create a sharded collection with two chunks on shard0, split at the key {x: -1}.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collection.getFullName(), key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: collection.getFullName(), middle: {x: -1}}));

// Insert some documents into the collection, but only into the higher of the two chunks.
assert.commandWorked(collection.insert(Array.from({length: 100}, (_, i) => ({_id: i, x: i}))));

// Enable the failpoint to cause range deletion to hang indefinitely.
const suspendRangeDeletionFailpoint = configureFailPoint(st.shard0, "suspendRangeDeletion");

// Note: Use _waitForDelete=false to ensure the command completes since the test intentionally
// causes range deletion to hang.
assert.commandWorked(st.s.adminCommand({
    moveChunk: collection.getFullName(),
    find: {x: 1},
    to: st.shard1.shardName,
    _waitForDelete: false,
}));

let res = assert.commandWorked(collection.update({x: 0}, {$set: {y: 1}}));
assert.eq(1, res.nMatched, res);
assert.eq(1, res.nModified, res);

// Do a multi=true update that will target both shards but not update any documents on the shard
// which owns the range [-1, MaxKey].
res = assert.commandWorked(
    collection.update({x: {$lte: 0}, y: {$exists: false}}, {$set: {x: -10, y: 2}}, {multi: true}));
assert.eq(0, res.nMatched, res);
assert.eq(0, res.nModified, res);
assert.eq(0, res.nUpserted, res);

// Wait for range deletion to happen on the donor.
suspendRangeDeletionFailpoint.off();
assert.soon(() => {
    return st.shard0.getCollection(collection.getFullName()).findOne({x: 1}) === null;
});

// Run a $out aggregation as a simple way to check for duplicate _id values.
assert.doesNotThrow(() => collection.aggregate([{$out: "output"}]));

st.stop();
})();
