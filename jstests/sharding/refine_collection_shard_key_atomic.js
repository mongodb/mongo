//
// Tests that refineCollectionShardKey atomically updates metadata in config.collections,
// config.chunks, and config.tags.
//

(function() {
'use strict';
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const kDbName = 'db';
const kNsName = kDbName + '.foo';
const kConfigCollections = 'config.collections';
const kConfigChunks = 'config.chunks';
const kConfigTags = 'config.tags';

const oldKeyDoc = {
    a: 1,
    b: 1
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1
};

jsTestLog('********** TEST TRANSACTION PASSES **********');

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// Ensure that there exist three chunks belonging to 'db.foo' covering the entire key range.
//
// Chunk 1: {a: MinKey, b: MinKey} -->> {a: 0, b: 0}
// Chunk 2: {a: 0, b: 0} -->> {a: 5, b: 5}
// Chunk 3: {a: 5, b: 5} -->> {a: MaxKey, b: MaxKey}
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 0, b: 0}}));
assert.commandWorked(mongos.adminCommand({split: kNsName, middle: {a: 5, b: 5}}));

// Ensure that there exist two zones belonging to 'db.foo' covering the entire key range.
//
// Zone 1: {a: MinKey, b: MinKey} -->> {a: 0, b: 0}
// Zone 2: {a: 0, b: 0} -->> {a: MaxKey, b: MaxKey}
assert.commandWorked(mongos.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));
assert.commandWorked(mongos.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_2'}));
assert.commandWorked(mongos.adminCommand(
    {updateZoneKeyRange: kNsName, min: {a: MinKey, b: MinKey}, max: {a: 0, b: 0}, zone: 'zone_1'}));
assert.commandWorked(mongos.adminCommand(
    {updateZoneKeyRange: kNsName, min: {a: 0, b: 0}, max: {a: MaxKey, b: MaxKey}, zone: 'zone_2'}));

// Verify that 'config.collections' is as expected before refineCollectionShardKey.
let oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(oldKeyDoc, oldCollArr[0].key);

// Verify that 'config.chunks' is as expected before refineCollectionShardKey.
const oldChunkArr =
    mongos.getCollection(kConfigChunks).find({ns: kNsName}).sort({min: 1}).toArray();
assert.eq(3, oldChunkArr.length);
assert.eq({a: MinKey, b: MinKey}, oldChunkArr[0].min);
assert.eq({a: 0, b: 0}, oldChunkArr[0].max);
assert.eq({a: 0, b: 0}, oldChunkArr[1].min);
assert.eq({a: 5, b: 5}, oldChunkArr[1].max);
assert.eq({a: 5, b: 5}, oldChunkArr[2].min);
assert.eq({a: MaxKey, b: MaxKey}, oldChunkArr[2].max);

// Verify that 'config.tags' is as expected before refineCollectionShardKey.
const oldTagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
assert.eq(2, oldTagsArr.length);
assert.eq({a: MinKey, b: MinKey}, oldTagsArr[0].min);
assert.eq({a: 0, b: 0}, oldTagsArr[0].max);
assert.eq({a: 0, b: 0}, oldTagsArr[1].min);
assert.eq({a: MaxKey, b: MaxKey}, oldTagsArr[1].max);

// Enable failpoint 'hangRefineCollectionShardKeyBeforeCommit' and run refineCollectionShardKey in a
// parallel shell.
assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyBeforeCommit', mode: 'alwaysOn'}));
let awaitShellToRefineCollectionShardKey = startParallelShell(() => {
    assert.commandWorked(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {a: 1, b: 1, c: 1, d: 1}}));
}, mongos.port);
waitForFailpoint('Hit hangRefineCollectionShardKeyBeforeCommit', 1);

// Verify that 'config.collections' has not been updated since we haven't committed the transaction.
let newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.sameMembers(oldCollArr, newCollArr);

// Verify that 'config.chunks' has not been updated since we haven't committed the transaction.
let newChunkArr = mongos.getCollection(kConfigChunks).find({ns: kNsName}).sort({min: 1}).toArray();
assert.sameMembers(oldChunkArr, newChunkArr);

// Verify that 'config.tags' has not been updated since we haven't committed the transaction.
let newTagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
assert.sameMembers(oldTagsArr, newTagsArr);

// Disable failpoint 'hangRefineCollectionShardKeyBeforeCommit' and await parallel shell.
assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyBeforeCommit', mode: 'off'}));
awaitShellToRefineCollectionShardKey();

// Verify that 'config.collections' is as expected after refineCollectionShardKey.
newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, newCollArr.length);
assert.eq(newKeyDoc, newCollArr[0].key);

// Verify that 'config.chunks' is as expected after refineCollectionShardKey.
newChunkArr = mongos.getCollection(kConfigChunks).find({ns: kNsName}).sort({min: 1}).toArray();
assert.eq(3, newChunkArr.length);
assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, newChunkArr[0].min);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, newChunkArr[0].max);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, newChunkArr[1].min);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, newChunkArr[1].max);
assert.eq({a: 5, b: 5, c: MinKey, d: MinKey}, newChunkArr[2].min);
assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, newChunkArr[2].max);

// Verify that 'config.tags' is as expected after refineCollectionShardKey.
newTagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
assert.eq(2, newTagsArr.length);
assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, newTagsArr[0].min);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, newTagsArr[0].max);
assert.eq({a: 0, b: 0, c: MinKey, d: MinKey}, newTagsArr[1].min);
assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, newTagsArr[1].max);

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** TEST TRANSACTION FAILS **********');

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// Verify that 'config.collections' is as expected before refineCollectionShardKey.
oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(oldKeyDoc, oldCollArr[0].key);

// Enable failpoint 'hangRefineCollectionShardKeyBeforeUpdatingChunks' and run
// refineCollectionShardKey in a parallel shell.
assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyBeforeUpdatingChunks', mode: 'alwaysOn'}));
awaitShellToRefineCollectionShardKey = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {a: 1, b: 1, c: 1, d: 1}}),
        ErrorCodes.WriteConflict);
}, mongos.port);
waitForFailpoint('Hit hangRefineCollectionShardKeyBeforeUpdatingChunks', 1);

// Manually write to 'config.chunks' to force refineCollectionShardKey to throw a WriteConflict
// exception.
assert.writeOK(mongos.getCollection(kConfigChunks).update({ns: kNsName}, {jumbo: true}));

// Disable failpoint 'hangRefineCollectionShardKeyBeforeUpdatingChunks' and await parallel shell.
assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {configureFailPoint: 'hangRefineCollectionShardKeyBeforeUpdatingChunks', mode: 'off'}));
awaitShellToRefineCollectionShardKey();

// Verify that 'config.collections' is as expected after refineCollectionShardKey.
newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.sameMembers(oldCollArr, newCollArr);

st.stop();
})();