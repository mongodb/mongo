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

const st = new ShardingTest({shards: 3});

const db = st.s.getDB(jsTestName());
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

const shardedColl1 = db.sharded_1;
const shardedColl2 = db.sharded_2;
const unshardedColl = db.unsharded;

assert.commandWorked(shardedColl1.insert({_id: 0}));
assert.commandWorked(shardedColl2.insert({_id: 1}));
assert.commandWorked(unshardedColl.insert({_id: 2}));

const uuid = function(coll) {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

assert.commandWorked(
    st.s.adminCommand({shardCollection: shardedColl1.getFullName(), key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: shardedColl2.getFullName(), key: {_id: 1}}));

// Move shardedColl1's chunk to shard1 and shardedColl2's chunk to shard2.
assert.commandWorked(st.s.adminCommand(
    {moveChunk: shardedColl1.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: shardedColl2.getFullName(), find: {_id: 0}, to: st.shard2.shardName}));

// Cannot use the collectionUUID parameter with a pipeline that executes entirely on mongos.
assert.commandFailedWithCode(db.adminCommand({
    aggregate: 1,
    pipeline: [{$currentOp: {localOps: true}}],
    cursor: {},
    collectionUUID: uuid(unshardedColl),
}),
                             6487500);

// Run an aggregate which will only target shard1, since shard0 and shard2 do not own any chunks.
let res = assert.commandFailedWithCode(db.runCommand({
    aggregate: shardedColl1.getName(),
    pipeline: [{$indexStats: {}}],
    cursor: {},
    collectionUUID: uuid(unshardedColl),
}),
                                       ErrorCodes.CollectionUUIDMismatch);
jsTestLog('$indexStats result: ' + tojson(res));
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(unshardedColl));
assert.eq(res.expectedCollection, shardedColl1.getName());
assert.eq(res.actualCollection, unshardedColl.getName());

res = assert.commandFailedWithCode(db.runCommand({
    aggregate: shardedColl1.getName(),
    pipeline: [{$indexStats: {}}],
    cursor: {},
    collectionUUID: uuid(shardedColl2),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
jsTestLog('$indexStats result: ' + tojson(res));
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(shardedColl2));
assert.eq(res.expectedCollection, shardedColl1.getName());
assert.eq(res.actualCollection, shardedColl2.getName());

// Run an aggregate which will only target shard1, since that is the shard on which the chunk
// containing the matching document lives.
res = assert.commandFailedWithCode(db.runCommand({
    aggregate: shardedColl1.getName(),
    pipeline: [{$match: {_id: 0}}],
    cursor: {},
    collectionUUID: uuid(unshardedColl),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
jsTestLog('$match result: ' + tojson(res));
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(unshardedColl));
assert.eq(res.expectedCollection, shardedColl1.getName());
assert.eq(res.actualCollection, unshardedColl.getName());

res = assert.commandFailedWithCode(db.runCommand({
    aggregate: shardedColl1.getName(),
    pipeline: [{$match: {_id: 0}}],
    cursor: {},
    collectionUUID: uuid(shardedColl2),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
jsTestLog('$match result: ' + tojson(res));
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(shardedColl2));
assert.eq(res.expectedCollection, shardedColl1.getName());
assert.eq(res.actualCollection, shardedColl2.getName());

st.stop();
})();
