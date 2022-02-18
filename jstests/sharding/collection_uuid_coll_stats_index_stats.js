/**
 * Tests the collectionUUID parameter of the aggregate command for $indexStats and $collStats
 * pipelines on sharded collection.
 * @tags: [requires_fcv_60]
 */

(function() {
'use strict';

load("jstests/libs/write_concern_util.js");  // For 'shardCollectionWithChunks'

const validateErrorResponse = function(
    res, db, collectionUUID, expectedCollection, actualCollection) {
    assert.eq(res.db, db);
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, expectedCollection);
    assert.eq(res.actualCollection, actualCollection);
};

const getUUID = function(database, collName) {
    return assert.commandWorked(database.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === collName)
        .info.uuid;
};

const st = new ShardingTest({
    name: jsTestName(),
    shards: 2,
    rs: {setParameter: {logComponentVerbosity: tojson({command: {verbosity: 1}})}}
});

const testDB = st.s.getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const shardedCollection = testDB["shardedCollection"];
// Set up sharded collection. Put 5 documents on each shard, with keys {x: 0...9}.
shardCollectionWithChunks(st, shardedCollection, 10 /* numDocs */);

const sameShardColl = testDB["sameShardCollection"];
assert.commandWorked(sameShardColl.insert({x: 1, y: 1}));

const otherDB = testDB.getSiblingDB("otherDB");
assert.commandWorked(otherDB.dropDatabase());
const otherShardColl = otherDB["otherShardColl"];
assert.commandWorked(otherShardColl.insert({x: 1, y: 1}));

// Make sure that the primary shard is different for the shardedCollection and the otherShardColl.
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
st.ensurePrimaryShard(otherDB.getName(), st.shard1.shardName);

const otherShardUUID = getUUID(otherDB, otherShardColl.getName());
const shardedUUID = getUUID(testDB, shardedCollection.getName());
const sameShardUUID = getUUID(testDB, sameShardColl.getName());

const testCommand = function(cmd, cmdObj) {
    jsTestLog("The command '" + cmd +
              "' succeeds on a sharded collection when the correct UUID is provided.");
    cmdObj[cmd] = shardedCollection.getName();
    cmdObj["collectionUUID"] = shardedUUID;
    assert.commandWorked(testDB.runCommand(cmdObj));

    jsTestLog("The command '" + cmd +
              "' fails on sharded collection when the provided UUID corresponds to a different " +
              "collection:");

    jsTestLog("If the aggregation command hits all shards, then it should return the " +
              "actual namespace of the unsharded collection that has same primary shard.");
    cmdObj["collectionUUID"] = sameShardUUID;
    let res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(
        res, testDB.getName(), sameShardUUID, shardedCollection.getName(), sameShardColl.getName());

    jsTestLog("If the aggregation command hits all shards, then it should return the " +
              "actual namespace of the unsharded collection that has different primary shard.");
    cmdObj["collectionUUID"] = otherShardUUID;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, testDB.getName(), otherShardUUID, shardedCollection.getName(), null);

    jsTestLog(
        "If the aggregation command hits only one shards, then it can't find the actual namespace" +
        " when the provided UUID corresponds to an unsharded collection that has different " +
        "primary shard.");
    cmdObj[cmd] = sameShardColl.getName();
    cmdObj["collectionUUID"] = otherShardUUID;
    res =
        assert.commandFailedWithCode(testDB.runCommand(cmdObj), ErrorCodes.CollectionUUIDMismatch);
    validateErrorResponse(res, testDB.getName(), otherShardUUID, sameShardColl.getName(), null);
};

testCommand("aggregate", {aggregate: "", pipeline: [{$collStats: {latencyStats: {}}}], cursor: {}});
testCommand("aggregate", {aggregate: "", pipeline: [{$indexStats: {}}], cursor: {}});

st.stop();
})();
