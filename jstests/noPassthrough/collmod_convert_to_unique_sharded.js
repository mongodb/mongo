/**
 * Tests collMod unique conversion ensures consistent index specs across all shards.
 *
 * @tags: [
 *   # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *   requires_persistence,
 *   requires_sharding,
 * ]
 */
(function() {
'use strict';

function countUniqueIndexes(coll, key) {
    const all = coll.getIndexes().filter(function(z) {
        return z.unique && friendlyEqual(z.key, key);
    });
    return all.length;
}

function countPrepareUniqueIndexes(coll, key) {
    const all = coll.getIndexes().filter(function(z) {
        return z.prepareUnique && friendlyEqual(z.key, key);
    });
    return all.length;
}

const st = new ShardingTest({shards: 2});
const mongos = st.s;

const db = mongos.getDB(jsTestName());
assert.commandWorked(
    mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.name}));

const shardedColl = db.sharded;

assert.commandWorked(
    mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: {a: 1}}));

// Move {a: 1} to shard0 and {a: 2} to shard1.
assert.commandWorked(st.splitAt(shardedColl.getFullName(), {a: 2}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: shardedColl.getFullName(), find: {a: 1}, to: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: shardedColl.getFullName(), find: {a: 2}, to: st.shard1.shardName}));

assert.commandWorked(shardedColl.createIndex({a: 1}));

assert.commandWorked(shardedColl.insert({_id: 0, a: 1}));
assert.commandWorked(shardedColl.insert({_id: 1, a: 2}));
assert.commandWorked(shardedColl.insert({_id: 2, a: 2}));

// Setting the indexes to 'prepareUnique' ensures no new duplicates will be inserted.
assert.commandWorked(db.runCommand(
    {collMod: shardedColl.getName(), index: {keyPattern: {a: 1}, prepareUnique: true}}));
assert.commandFailedWithCode(shardedColl.insert({_id: 3, a: 1}), ErrorCodes.DuplicateKey);
assert.commandFailedWithCode(shardedColl.insert({_id: 4, a: 2}), ErrorCodes.DuplicateKey);

// Try converting the index to unique and make sure no indexes are converted on any shards.
assert.commandFailedWithCode(
    db.runCommand({collMod: shardedColl.getName(), index: {keyPattern: {a: 1}, unique: true}}),
    ErrorCodes.CannotConvertIndexToUnique);

const s0Coll = st.shard0.getDB(jsTestName()).getCollection("sharded");
const s1Coll = st.shard1.getDB(jsTestName()).getCollection("sharded");
assert.eq(countUniqueIndexes(s0Coll, {a: 1}),
          0,
          'index should not be unique: ' + tojson(s0Coll.getIndexes()));
assert.eq(countUniqueIndexes(s1Coll, {a: 1}),
          0,
          'index should not be unique: ' + tojson(s1Coll.getIndexes()));
assert.eq(countPrepareUniqueIndexes(s0Coll, {a: 1}),
          1,
          'index should be prepareUnique: ' + tojson(s0Coll.getIndexes()));
assert.eq(countPrepareUniqueIndexes(s1Coll, {a: 1}),
          1,
          'index should be prepareUnique: ' + tojson(s1Coll.getIndexes()));

// Remove the duplicate and confirm the indexes are converted.
assert.commandWorked(shardedColl.deleteOne({_id: 2}));
assert.commandWorked(
    db.runCommand({collMod: shardedColl.getName(), index: {keyPattern: {a: 1}, unique: true}}));
assert.eq(countUniqueIndexes(s0Coll, {a: 1}),
          1,
          'index should be unique: ' + tojson(s0Coll.getIndexes()));
assert.eq(countUniqueIndexes(s1Coll, {a: 1}),
          1,
          'index should be unique: ' + tojson(s1Coll.getIndexes()));

st.stop();
})();
