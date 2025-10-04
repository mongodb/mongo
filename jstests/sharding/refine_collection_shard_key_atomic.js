//
// Tests that refineCollectionShardKey atomically updates metadata in config.collections,
// config.chunks, and config.tags.
//

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const kDbName = "db";
const kNsName = kDbName + ".foo";
const kConfigCollections = "config.collections";
const kConfigChunks = "config.chunks";
const kConfigTags = "config.tags";

const oldKeyDoc = {
    a: 1,
    b: 1,
};
const newKeyDoc = {
    a: 1,
    b: 1,
    c: 1,
    d: 1,
};

jsTestLog("********** TEST TRANSACTION PASSES **********");

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
assert.commandWorked(mongos.adminCommand({addShardToZone: st.shard0.shardName, zone: "zone_1"}));
assert.commandWorked(mongos.adminCommand({addShardToZone: st.shard0.shardName, zone: "zone_2"}));
assert.commandWorked(
    mongos.adminCommand({updateZoneKeyRange: kNsName, min: {a: MinKey, b: MinKey}, max: {a: 0, b: 0}, zone: "zone_1"}),
);
assert.commandWorked(
    mongos.adminCommand({updateZoneKeyRange: kNsName, min: {a: 0, b: 0}, max: {a: MaxKey, b: MaxKey}, zone: "zone_2"}),
);

// Verify that 'config.collections' is as expected before refineCollectionShardKey.
let oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(oldKeyDoc, oldCollArr[0].key);

// Verify that 'config.chunks' is as expected before refineCollectionShardKey.
const oldChunkArr = findChunksUtil.findChunksByNs(mongos.getDB("config"), kNsName).sort({min: 1}).toArray();
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
let hangBeforeCommitFailPoint = configureFailPoint(
    st.configRS.getPrimary(),
    "hangRefineCollectionShardKeyBeforeCommit",
);

let awaitShellToRefineCollectionShardKey = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({refineCollectionShardKey: "db.foo", key: {a: 1, b: 1, c: 1, d: 1}}));
}, mongos.port);
hangBeforeCommitFailPoint.wait();

// Verify that 'config.collections' has not been updated since we haven't committed the transaction,
// except for the 'allowMigrations' property which is updated by the
// RefineCollectionShardKeyCoordinator before the commit phase.
let newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
newCollArr.forEach((element) => {
    delete element["allowMigrations"];
});
assert.sameMembers(oldCollArr, newCollArr);

// Verify that 'config.chunks' has not been updated since we haven't committed the transaction,
// except for the chunk version which has been bumped by the setAllowMigrations command prior to the
// refineCollectionShardKey commit.
let newChunkArr = findChunksUtil.findChunksByNs(mongos.getDB("config"), kNsName).sort({min: 1}).toArray();

newChunkArr.forEach((element) => {
    delete element["lastmod"];
});

let oldChunkArrWithoutLastmod = oldChunkArr;
oldChunkArrWithoutLastmod.forEach((element) => {
    delete element["lastmod"];
});

assert.sameMembers(oldChunkArr, newChunkArr);

// Verify that 'config.tags' has not been updated since we haven't committed the transaction.
let newTagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
assert.sameMembers(oldTagsArr, newTagsArr);

// Disable failpoint 'hangRefineCollectionShardKeyBeforeCommit' and await parallel shell.
hangBeforeCommitFailPoint.off();
awaitShellToRefineCollectionShardKey();

// Verify that 'config.collections' is as expected after refineCollectionShardKey.
newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, newCollArr.length);
assert.eq(newKeyDoc, newCollArr[0].key);

// Verify that 'config.chunks' is as expected after refineCollectionShardKey.
newChunkArr = findChunksUtil.findChunksByNs(mongos.getDB("config"), kNsName).sort({min: 1}).toArray();
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

jsTestLog("********** TEST TRANSACTION AUTOMATICALLY RETRIES **********");

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// Verify that 'config.collections' is as expected before refineCollectionShardKey.
oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(oldKeyDoc, oldCollArr[0].key);

// Enable failpoint 'hangRefineCollectionShardKeyBeforeUpdatingChunks' and run
// refineCollectionShardKey in a parallel shell.
let hangBeforeUpdatingChunksFailPoint = configureFailPoint(
    st.configRS.getPrimary(),
    "hangRefineCollectionShardKeyBeforeUpdatingChunks",
);
awaitShellToRefineCollectionShardKey = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({refineCollectionShardKey: "db.foo", key: {a: 1, b: 1, c: 1, d: 1}}));
}, mongos.port);
hangBeforeUpdatingChunksFailPoint.wait();

// Manually write to 'config.chunks' to force refineCollectionShardKey to throw a WriteConflict
// exception.
const coll = mongos.getCollection(kNsName);
if (coll.timestamp) {
    assert.writeOK(mongos.getCollection(kConfigChunks).update({uuid: coll.uuid}, {$set: {jumbo: true}}));
} else {
    assert.writeOK(mongos.getCollection(kConfigChunks).update({ns: kNsName}, {$set: {jumbo: true}}));
}

// Disable failpoint 'hangRefineCollectionShardKeyBeforeUpdatingChunks' and await parallel shell.
hangBeforeUpdatingChunksFailPoint.off();
awaitShellToRefineCollectionShardKey();

// Verify that 'config.collections' is as expected after refineCollectionShardKey.
newCollArr = mongos.getCollection(kConfigCollections).find({_id: kNsName}).toArray();
assert.eq(1, newCollArr.length);
assert.eq(newKeyDoc, newCollArr[0].key);

// Verify that 'config.chunks' is as expected after refineCollectionShardKey.
newChunkArr = findChunksUtil.findChunksByNs(mongos.getDB("config"), kNsName).sort({min: 1}).toArray();
assert.eq(1, newChunkArr.length);
assert.eq({a: MinKey, b: MinKey, c: MinKey, d: MinKey}, newChunkArr[0].min);
assert.eq({a: MaxKey, b: MaxKey, c: MaxKey, d: MaxKey}, newChunkArr[0].max);

// Verify that 'config.tags' is as expected after refineCollectionShardKey.
newTagsArr = mongos.getCollection(kConfigTags).find({ns: kNsName}).sort({min: 1}).toArray();
assert.eq([], newTagsArr);

st.stop();
