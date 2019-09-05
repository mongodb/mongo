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

const st = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1}});
const testDB = st.s0.getDB("mrShard");
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

function verifyOutput(mrOutput, expectedNOutputDocs) {
    assert.commandWorked(mrOutput);
    assert.eq(mrOutput.counts.input, nTotalDocs, `input count is wrong: ${tojson(mrOutput)}`);
    assert.eq(mrOutput.counts.emit, nTotalDocs, `emit count is wrong: ${tojson(mrOutput)}`);
    assert.gt(
        mrOutput.counts.reduce, nValuesPerKey - 1, `reduce count is wrong: ${tojson(mrOutput)}`);
    assert.eq(
        mrOutput.counts.output, expectedNOutputDocs, `output count is wrong: ${tojson(mrOutput)}`);
}

function mapFn() {
    emit(this.key, 1);
}
function reduceFn(key, values) {
    return Array.sum(values);
}

(function testShardedOutput() {
    // Check that merge to an existing empty sharded collection works and creates a new UUID after
    // M/R
    const outputColl = testDB[inputColl.getName() + "Out"];
    st.adminCommand({shardCollection: outputColl.getFullName(), key: {_id: 1}});
    let origUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    let out = testDB.srcSharded.mapReduce(
        mapFn, reduceFn, {out: {merge: outputColl.getName(), sharded: true}});
    verifyOutput(out, nDistinctKeys);
    let newUUID = getUUIDFromConfigCollections(st.s, outputColl.getFullName());
    assert.neq(origUUID, newUUID);

    // Shard1 is the primary shard and only one chunk should have been written, so the chunk with
    // the new UUID should have been written to it.
    assert.eq(newUUID,
              getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), outputColl.getName()));

    // Shard0 should not have any chunks from the output collection because all shards should have
    // returned an empty split point list in the first phase of the mapReduce, since the reduced
    // data size is far less than the chunk size setting of 1MB.
    assertCollectionNotOnShard(st.shard0.getDB(testDB.getName()), outputColl.getName());

    // Check that merge to an existing sharded collection that has data on all shards works and that
    // the collection uses the same UUID after M/R
    st.adminCommand({split: outputColl.getFullName(), middle: {"_id": 2000}});
    st.adminCommand(
        {moveChunk: outputColl.getFullName(), find: {"_id": 2000}, to: st.shard0.shardName});
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

    // Check that replace to an existing sharded collection has data on all shards works and that
    // the collection creates a new UUID after M/R.
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
    // Check that reduce to an existing unsharded collection fails when `sharded: true`.
    const reduceOutput = testDB.reduceUnsharded;
    assert.commandWorked(testDB.runCommand({create: reduceOutput.getName()}));
    assert.commandFailed(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: {reduce: reduceOutput.getName(), sharded: true}
    }));

    assert.commandWorked(testDB.reduceUnsharded.insert({x: 1}));
    assert.commandFailed(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: {reduce: reduceOutput.getName(), sharded: true}
    }));

    // Check that replace to an existing unsharded collection works when `sharded: true`.
    const replaceOutput = testDB.replaceUnsharded;
    assert.commandWorked(testDB.runCommand({create: replaceOutput.getName()}));
    let origUUID =
        getUUIDFromListCollections(st.s.getDB(testDB.getName()), replaceOutput.getName());

    assert.commandWorked(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: {replace: replaceOutput.getName(), sharded: true}
    }));

    let newUUID = getUUIDFromConfigCollections(st.s, replaceOutput.getFullName());
    assert.neq(origUUID, newUUID);
    assert.eq(
        newUUID,
        getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), replaceOutput.getName()));

    assert.commandWorked(testDB.replaceUnsharded.insert({x: 1}));
    origUUID = getUUIDFromListCollections(st.s.getDB(testDB.getName()), replaceOutput.getName());

    assert.commandWorked(testDB.runCommand({
        mapReduce: inputColl.getName(),
        map: mapFn,
        reduce: reduceFn,
        out: {replace: replaceOutput.getName(), sharded: true}
    }));

    newUUID = getUUIDFromConfigCollections(st.s, replaceOutput.getFullName());
    assert.neq(origUUID, newUUID);
    assert.eq(
        newUUID,
        getUUIDFromListCollections(st.shard1.getDB(testDB.getName()), replaceOutput.getName()));
}());

st.stop();
})();
