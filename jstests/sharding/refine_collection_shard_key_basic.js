//
// Basic tests for refineCollectionShardKey.
//

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    flushRoutersAndRefreshShardMetadata
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    simpleValidationTests,
    shardKeyValidationTests,
    uniquePropertyTests,
    integrationTests,
    dropAndReshardColl
} from "jstests/sharding/libs/refine_collection_shard_key_common.js";

const st = new ShardingTest({
    mongos: 2,
    shards: 2,
    rs: {nodes: 3},
    configOptions:
        {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}}
});

const mongos = st.s0;
const staleMongos = st.s1;
const primaryShard = st.shard0.shardName;
const secondaryShard = st.shard1.shardName;
const kDbName = 'db';
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;
const kUnrelatedName = kDbName + '.bar';
const kConfigCollections = 'config.collections';
const kConfigTags = 'config.tags';

function dropAndRecreateColl(keyDoc) {
    assert.commandWorked(mongos.getDB(kDbName).runCommand({drop: kCollName}));
    assert.commandWorked(mongos.getCollection(kNsName).insert(keyDoc));
}

// 1. Assume oldKeyDoc = {a: 1, b: 1} when validating operations before
//    'refineCollectionShardKey'.
// 2. Assume newKeyDoc = {a: 1, b: 1, c: 1, d: 1} when validating operations after
//    'refineCollectionShardKey'.

function setupCRUDBeforeRefine() {
    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // The documents below will be read after refineCollectionShardKey to verify data integrity.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 5, b: 5, c: 5, d: 5}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 10, b: 10, c: 10, d: 10}));
}

function validateCRUDAfterRefine() {
    // Force a refresh on the mongos and each shard because refineCollectionShardKey only triggers
    // best-effort shard refreshes.
    flushRoutersAndRefreshShardMetadata(st, {ns: kNsName});

    const session = mongos.startSession({retryWrites: true});
    const sessionDB = session.getDatabase(kDbName);

    // Verify that documents inserted before refineCollectionShardKey have not been corrupted.
    assert.eq([{a: 5, b: 5, c: 5, d: 5}],
              sessionDB.getCollection(kCollName).find({a: 5}, {_id: 0}).toArray());
    assert.eq([{a: 10, b: 10, c: 10, d: 10}],
              sessionDB.getCollection(kCollName).find({a: 10}, {_id: 0}).toArray());

    // A write with the incomplete shard key is treated as if the missing values are null.

    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1}));

    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: 1, b: 1, c: null, d: null}));
    assert.neq(null, sessionDB.getCollection(kCollName).findOne({a: -1, b: -1, c: null, d: null}));

    // Full shard key writes work properly.
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: 1, b: 1, c: 1, d: 1}));
    assert.commandWorked(sessionDB.getCollection(kCollName).insert({a: -1, b: -1, c: -1, d: -1}));

    // This enables the feature allows writes to omit the shard key in their queries.
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1}, {$set: {x: 2}}));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: 1, b: 1, c: 1, d: 1}, {$set: {b: 2}}));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).update({a: -1, b: -1, c: -1, d: -1}, {$set: {b: 4}}));

    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);

    // Versioned reads against secondaries should work as expected.
    mongos.setReadPref("secondary");
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).x);
    assert.eq(2, sessionDB.getCollection(kCollName).findOne({c: 1}).b);
    assert.eq(4, sessionDB.getCollection(kCollName).findOne({c: -1}).b);
    mongos.setReadPref(null);

    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 1, b: 1}, true));
    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: -1, b: -1}, true));

    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 1, b: 2, c: 1, d: 1}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: -1, b: 4, c: -1, d: -1}, true));
    assert.commandWorked(sessionDB.getCollection(kCollName).remove({a: 5, b: 5, c: 5, d: 5}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: 10, b: 10, c: 10, d: 10}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: 1, b: 1, c: null, d: null}, true));
    assert.commandWorked(
        sessionDB.getCollection(kCollName).remove({a: -1, b: -1, c: null, d: null}, true));

    assert.eq(null, sessionDB.getCollection(kCollName).findOne());
}

function validateUnrelatedCollAfterRefine(oldCollArr, oldChunkArr, oldTagsArr) {
    const collArr = mongos.getCollection(kConfigCollections).find({_id: kUnrelatedName}).toArray();
    assert.eq(1, collArr.length);
    assert.sameMembers(oldCollArr, collArr);

    const chunkArr =
        findChunksUtil.findChunksByNs(mongos.getDB('config'), kUnrelatedName).toArray();
    assert.eq(3, chunkArr.length);
    assert.sameMembers(oldChunkArr, chunkArr);

    const tagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
    assert.eq(3, tagsArr.length);
    assert.sameMembers(oldTagsArr, tagsArr);
}

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

simpleValidationTests(mongos, kDbName);
shardKeyValidationTests(mongos, kDbName);
uniquePropertyTests(mongos, kDbName);

jsTestLog('********** NAMESPACE VALIDATION TESTS **********');

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: {_id: 1}}));

// Configure failpoint 'hangRefineCollectionShardKeyAfterRefresh' on staleMongos and run
// refineCollectionShardKey against this mongos in a parallel thread.
let hangAfterRefreshFailPoint =
    configureFailPoint(staleMongos, 'hangRefineCollectionShardKeyAfterRefresh');
const awaitShellToTriggerNamespaceNotSharded = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand({refineCollectionShardKey: 'db.foo', key: {_id: 1, aKey: 1}}),
        ErrorCodes.NamespaceNotSharded);
}, staleMongos.port);
hangAfterRefreshFailPoint.wait();

// Drop and re-create namespace 'db.foo' without staleMongos refreshing its metadata.
dropAndRecreateColl({aKey: 1});

// Should fail because namespace 'db.foo' is not sharded.
hangAfterRefreshFailPoint.off();
awaitShellToTriggerNamespaceNotSharded();

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('********** INTEGRATION TESTS **********');
integrationTests(mongos, kDbName, primaryShard, secondaryShard);

assert.commandWorked(mongos.adminCommand({shardCollection: kNsName, key: oldKeyDoc}));
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

// CRUD operations before and after refineCollectionShardKey should work as expected.
setupCRUDBeforeRefine();
assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateCRUDAfterRefine();

// Create an unrelated namespace 'db.bar' with 3 chunks and 3 tags to verify that it isn't
// corrupted after refineCollectionShardKey.
dropAndReshardColl(mongos, kDbName, kCollName, oldKeyDoc);
assert.commandWorked(mongos.getCollection(kNsName).createIndex(newKeyDoc));

assert.commandWorked(mongos.adminCommand({shardCollection: kUnrelatedName, key: oldKeyDoc}));
assert.commandWorked(mongos.adminCommand({split: kUnrelatedName, middle: {a: 0, b: 0}}));
assert.commandWorked(mongos.adminCommand({split: kUnrelatedName, middle: {a: 5, b: 5}}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_1'}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_2'}));
assert.commandWorked(mongos.adminCommand({addShardToZone: primaryShard, zone: 'unrelated_3'}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: MinKey, b: MinKey},
    max: {a: 0, b: 0},
    zone: 'unrelated_1'
}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: 0, b: 0},
    max: {a: 5, b: 5},
    zone: 'unrelated_2'
}));
assert.commandWorked(mongos.adminCommand({
    updateZoneKeyRange: kUnrelatedName,
    min: {a: 5, b: 5},
    max: {a: MaxKey, b: MaxKey},
    zone: 'unrelated_3'
}));

const oldCollArr = mongos.getCollection(kConfigCollections).find({_id: kUnrelatedName}).toArray();
const oldChunkArr = findChunksUtil.findChunksByNs(mongos.getDB('config'), kUnrelatedName).toArray();
const oldTagsArr = mongos.getCollection(kConfigTags).find({ns: kUnrelatedName}).toArray();
assert.eq(1, oldCollArr.length);
assert.eq(3, oldChunkArr.length);
assert.eq(3, oldTagsArr.length);

assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: kNsName, key: newKeyDoc}));
validateUnrelatedCollAfterRefine(oldCollArr, oldChunkArr, oldTagsArr);

// Assumes the given arrays are sorted by the max field.
function compareMinAndMaxFields(shardedArr, refinedArr) {
    assert(shardedArr.length && refinedArr.length, tojson(shardedArr) + ", " + tojson(refinedArr));
    assert.eq(shardedArr.length, refinedArr.length, tojson(shardedArr) + ", " + tojson(refinedArr));

    const shardedMinAndMax = shardedArr.map(obj => {
        return {min: obj.min, max: obj.max};
    });
    const refinedMinAndMax = refinedArr.map(obj => {
        return {min: obj.min, max: obj.max};
    });
    assert.eq(shardedMinAndMax, refinedMinAndMax);
}

// Verifies the min and max fields are the same for the chunks and tags in the given collections.
function compareBoundaries(conn, shardedNs, refinedNs) {
    // Compare chunks.
    const shardedChunks =
        findChunksUtil.findChunksByNs(conn.getDB("config"), shardedNs).sort({max: 1}).toArray();
    const refinedChunks =
        findChunksUtil.findChunksByNs(conn.getDB("config"), refinedNs).sort({max: 1}).toArray();
    compareMinAndMaxFields(shardedChunks, refinedChunks);

    // Compare tags.
    const shardedTags = conn.getDB("config").tags.find({ns: shardedNs}).sort({max: 1}).toArray();
    const refinedTags = conn.getDB("config").tags.find({ns: refinedNs}).sort({max: 1}).toArray();
    compareMinAndMaxFields(shardedTags, refinedTags);
}

//
// Verify the chunk and tag boundaries are the same for a collection sharded to a certain shard key
// and a collection refined to that same shard key.
//

// For a shard key without nested fields.
(() => {
    const dbName = "compareDB";
    const shardedNs = dbName + ".shardedColl";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: {a: 1, b: 1, c: 1}}));
    assert.commandWorked(
        st.s.adminCommand({split: shardedNs, middle: {a: 0, b: MinKey, c: MinKey}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {a: MinKey, b: MinKey, c: MinKey},
        max: {a: 0, b: MinKey, c: MinKey},
        zone: 'zone_1'
    }));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {a: 10, b: MinKey, c: MinKey},
        max: {a: MaxKey, b: MaxKey, c: MaxKey},
        zone: 'zone_1'
    }));

    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {a: 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {a: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: MinKey}, max: {a: 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: 10}, max: {a: MaxKey}, zone: 'zone_1'}));

    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({a: 1, b: 1, c: 1}));
    assert.commandWorked(
        st.s.adminCommand({refineCollectionShardKey: refinedNs, key: {a: 1, b: 1, c: 1}}));

    compareBoundaries(st.s, shardedNs, refinedNs);
})();

// For a shard key with nested fields .
(() => {
    const dbName = "compareDBNested";
    const shardedNs = dbName + ".shardedColl";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: shardedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));
    assert.commandWorked(
        st.s.adminCommand({split: shardedNs, middle: {"a.b": 0, "c.d.e": MinKey, f: MinKey}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {"a.b": MinKey, "c.d.e": MinKey, f: MinKey},
        max: {"a.b": 0, "c.d.e": MinKey, f: MinKey},
        zone: 'zone_1'
    }));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: shardedNs,
        min: {"a.b": 10, "c.d.e": MinKey, f: MinKey},
        max: {"a.b": MaxKey, "c.d.e": MaxKey, f: MaxKey},
        zone: 'zone_1'
    }));

    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {"a.b": 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {"a.b": 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": MinKey}, max: {"a.b": 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": 10}, max: {"a.b": MaxKey}, zone: 'zone_1'}));

    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({"a.b": 1, "c.d.e": 1, f: 1}));
    assert.commandWorked(st.s.adminCommand(
        {refineCollectionShardKey: refinedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));

    compareBoundaries(st.s, shardedNs, refinedNs);
})();

st.stop();
