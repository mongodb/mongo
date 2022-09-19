/**
 * Tests that an index with unique or prepareUnique option cannot be built on a sharded collection
 * with an incompatible shard key, or the collection cannot be sharded with an incompatible
 * unique/prepareUnique index.
 *
 * @tags: [
 *   # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

const st = new ShardingTest({shards: 1, mongos: 1});
const dbName = "db";
const db = st.getDB(dbName);
const collNamePrefix = jsTestName();
let count = 0;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Cannot shard the collection with a conflicting prepareUnique index.
let coll = db[collNamePrefix + count++];
assert.commandWorked(coll.createIndex({a: 1}, {prepareUnique: 1}));
assert.commandFailedWithCode(st.s.adminCommand({shardCollection: coll.getFullName(), key: {b: 1}}),
                             ErrorCodes.InvalidOptions);
// Can shard the collection with a compatible prepareUnique index.
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

// Cannot shard the collection with a conflicting unique index.
coll = db[collNamePrefix + count++];
assert.commandWorked(coll.createIndex({a: 1}, {unique: 1}));
assert.commandFailedWithCode(st.s.adminCommand({shardCollection: coll.getFullName(), key: {b: 1}}),
                             ErrorCodes.InvalidOptions);
// Can shard the collection with a compatible unique index.
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

// Cannot create the index with a conflicting shard key.
coll = db[collNamePrefix + count++];
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {b: 1}}));
assert.commandFailedWithCode(coll.createIndex({a: 1}, {prepareUnique: 1}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({a: 1}, {unique: 1}), ErrorCodes.CannotCreateIndex);
// Can create the index with a compatible shard key.
assert.commandWorked(coll.createIndex({b: 1, a: 1}, {prepareUnique: 1}));
assert.commandWorked(coll.createIndex({b: 1, c: 1}, {unique: 1}));

// Cannot convert an index to prepareUnique with a conflicting shard key.
coll = db[collNamePrefix + count++];
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {b: 1}}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandFailedWithCode(
    db.runCommand({collMod: coll.getName(), index: {keyPattern: {a: 1}, prepareUnique: true}}),
    ErrorCodes.InvalidOptions);
// Can convert an index to prepareUnique and unique with a compatible shard key.
assert.commandWorked(coll.createIndex({b: 1, d: 1}));
assert.commandWorked(db.runCommand(
    {collMod: coll.getName(), index: {keyPattern: {b: 1, d: 1}, prepareUnique: true}}));
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), index: {keyPattern: {b: 1, d: 1}, unique: true}}));

st.stop();
})();
