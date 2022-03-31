/**
 * Tests the collectionUUID parameter of the aggregate command when not all shards own chunks for
 * the collection.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2});

const db = st.s.getDB(jsTestName());
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

const shardedColl = db.sharded;
const unshardedColl = db.unsharded;

assert.commandWorked(shardedColl.insert({_id: 0}));
assert.commandWorked(unshardedColl.insert({_id: 2}));

const uuid = function(coll) {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

assert.commandWorked(
    st.s.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

// Move the chunk to shard1.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: shardedColl.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

// Run an aggregate which will only target shard1, since shard0 does not own any chunks.
let res = assert.commandFailedWithCode(db.runCommand({
    aggregate: shardedColl.getName(),
    pipeline: [{$indexStats: {}}],
    cursor: {},
    collectionUUID: uuid(unshardedColl),
}),
                                       ErrorCodes.CollectionUUIDMismatch);
jsTestLog('$indexStats result: ' + tojson(res));
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(unshardedColl));
assert.eq(res.expectedCollection, shardedColl.getName());
assert.eq(res.actualCollection, unshardedColl.getName());

st.stop();
})();
