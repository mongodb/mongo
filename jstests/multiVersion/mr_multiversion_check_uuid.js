// Test that the UUID of the target collection of a mapReduce remains consistent between the config
// server and the shards. This is in the multiversion suite since SERVER-44527 is relevant for the
// pre-4.4 version of the mongod implementation of mapReduce, which still runs when the FCV is 4.2.

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";
load("jstests/libs/uuid_util.js");

function assertCollectionNotOnShard(db, coll) {
    const listCollsRes = db.runCommand({listCollections: 1, filter: {name: coll}});
    assert.commandWorked(listCollsRes);
    assert.neq(undefined, listCollsRes.cursor);
    assert.neq(undefined, listCollsRes.cursor.firstBatch);
    assert.eq(0, listCollsRes.cursor.firstBatch.length);
}

// Setup a sharded cluster with the last-stable mongos and the latest binVersion shards. This is
// meant to test the legacy code path in a multiversion cluster.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, binVersion: "latest"},
    other: {mongosOptions: {binVersion: "last-stable"}, chunkSize: 1}
});

const testDB = st.s0.getDB(jsTestName());
const inputColl = testDB.srcSharded;

st.adminCommand({enableSharding: testDB.getName()});
st.ensurePrimaryShard(testDB.getName(), st.shard1.shardName);
st.adminCommand({shardCollection: inputColl.getFullName(), key: {_id: 1}});

const nDistinctKeys = 512;
const nValuesPerKey = 100;
const nTotalDocs = nDistinctKeys * nValuesPerKey;

const bulk = inputColl.initializeUnorderedBulkOp();
for (let key = 0; key < nDistinctKeys; key++) {
    for (let value = 0; value < nValuesPerKey; value++) {
        bulk.insert({key: key, value: value});
    }
}
assert.commandWorked(bulk.execute());

const outputColl = testDB[inputColl.getName() + "_out"];

function verifyOutput(mrOutput, expectedNOutputDocs) {
    assert.commandWorked(mrOutput);
    assert.eq(expectedNOutputDocs, outputColl.find().itcount());
}

function mapFn() {
    emit(this.key, 1);
}
function reduceFn(key, values) {
    return Array.sum(values);
}

(function testShardedOutput() {
    // Check that merge to an existing empty sharded collection works and preserves the UUID after
    // M/R.
    st.adminCommand({shardCollection: outputColl.getFullName(), key: {_id: 1}});
    let origUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    let out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {merge: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys);
    let newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.eq(origUUID, newUUID);

    // Shard1 is the primary shard and only one chunk should have been written, so the chunk with
    // the new UUID should have been written to it.
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Shard0 should not have any chunks from the output collection because all shards should have
    // returned an empty split point list in the first phase of the mapReduce, since the reduced
    // data size is far less than the chunk size setting of 1MB.
    assertCollectionNotOnShard(st.shard0.getDB(testDB.getName()), outputColl.getName());

    // Shard and split the output collection, moving the chunk with {_id: 2000} to shard0. All data
    // from the result of the mapReduce will be directed to shard1.
    st.adminCommand({split: outputColl.getFullName(), middle: {"_id": 2000}});
    st.adminCommand(
        {moveChunk: outputColl.getFullName(), find: {"_id": 2000}, to: st.shard0.shardName});
    origUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());

    // Check that merge to an existing sharded collection that has data only on the primary shard
    // works and that the collection uses the same UUID after M/R.
    assert.commandWorked(outputColl.insert({_id: 1000}));
    out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {merge: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys + 1);

    newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.eq(origUUID, newUUID);
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard0.getDB(testDB.getName()), outputColl.getName()));
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Check that merge to an existing sharded collection that has data only on the non-primary
    // shard works and that the collection uses the same UUID after M/R.
    assert.commandWorked(outputColl.remove({}));
    assert.commandWorked(outputColl.insert({_id: 2001}));

    out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {merge: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys + 1);

    newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.eq(origUUID, newUUID);
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard0.getDB(testDB.getName()), outputColl.getName()));
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Check that merge to an existing sharded collection that has data on all shards works and that
    // the collection uses the same UUID after M/R.
    assert.commandWorked(outputColl.remove({}));
    assert.commandWorked(outputColl.insert([{_id: 1000}, {_id: 2001}]));

    origUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {merge: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys + 2);

    newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.eq(origUUID, newUUID);
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard0.getDB(testDB.getName()), outputColl.getName()));
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Similarly, check that reduce to an existing sharding collection that has data only on the
    // primary shard works and that the collection uses the same UUID after M/R.
    assert.commandWorked(outputColl.remove({}));
    assert.commandWorked(outputColl.insert({_id: 1000}));
    out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {reduce: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys + 1);

    newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.eq(origUUID, newUUID);
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard0.getDB(testDB.getName()), outputColl.getName()));
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Check that replace to an existing sharded collection has data on all shards works and that
    // the collection creates a new UUID after M/R.
    assert.commandWorked(outputColl.insert({_id: 2001}));
    origUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {replace: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys);

    newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.neq(origUUID, newUUID);

    // Shard1 is the primary shard and only one chunk should have been written, so the chunk with
    // the new UUID should have been written to it.
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Shard0 should not have any chunks from the output collection because all shards should have
    // returned an empty split point list in the first phase of the mapReduce, since the reduced
    // data size is far less than the chunk size setting of 1MB.
    assertCollectionNotOnShard(st.shard0.getDB(testDB.getName()), outputColl.getName());
}());

(function testUnshardedOutputColl() {
    // Check that replace with {sharded: true} to an existing unsharded collection works and creates
    // a sharded collection with a new UUID.
    const replaceOutput = testDB.replaceUnsharded;
    assert.commandWorked(testDB.runCommand({create: replaceOutput.getName()}));
    let origUUID =
        getUUIDFromListCollections(st.s.getDB(testDB.getName()), replaceOutput.getName());

    assert.commandWorked(testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {replace: replaceOutput.getName(), sharded: true}}));
    assert.eq(nDistinctKeys, replaceOutput.find().itcount());

    let newUUID = getUUIDFromConfigCollections(st.s, replaceOutput.getFullName());
    assert.neq(origUUID, newUUID);
    assert.eq(
        newUUID,
        getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), replaceOutput.getName()));

    // Shard0 (non-primary) should not be aware of the unsharded collection.
    assertCollectionNotOnShard(st.shard0.getDB(testDB.getName()), replaceOutput.getName());
}());

st.stop();
})();
