/**
 * Smoke tests for moveChunk command
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns
 * ]
 */

import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const configDB = db.getSiblingDB("config");
const shardNames = db.adminCommand({listShards: 1}).shards.map((shard) => shard._id);

if (shardNames.length < 2) {
    print(jsTestName() + " will not run; at least 2 shards are required.");
    quit();
}

// Finds and returns the initial chunk and the shard name it resides in, and one other shard name.
function getInitialChunkAndShardNames(configDB, ns, shardNames) {
    let chunk;
    // index to shard name that contains the chunk:
    let chunkIndex = null;
    // index to another shard name where the chunk can be moved to:
    let otherIndex = null;
    let i = 0;
    while (i < shardNames.length && chunk == null) {
        chunk = findChunksUtil.findOneChunkByNs(configDB, ns, {shard: shardNames[i]});
        if (chunk != null) {
            chunkIndex = i;
            if (chunkIndex > 0) {
                otherIndex = chunkIndex - 1;
            } else {
                otherIndex = chunkIndex + 1;
            }
        }
        ++i;
    }
    assert(chunkIndex != null && chunk != null && otherIndex != null);

    return [chunk, shardNames[chunkIndex], shardNames[otherIndex]];
}

function testErrorOnInvalidNamespace(db) {
    jsTestLog("Test moveChunk fails for invalid namespace.");
    assert.commandFailed(db.adminCommand({moveChunk: "", find: {_id: 1}, to: shardNames[1]}));
}

function testErrorOnDatabaseDNE(db) {
    jsTestLog("Test moveChunk fails for namespace that does not exist.");
    assert.commandFailed(db.adminCommand({moveChunk: "nonexistent.namespace", find: {_id: 1}, to: shardNames[1]}));
}

function testErrorOnUnshardedCollection(db) {
    jsTestLog("Test moveChunk fails for an unsharded collection.");
    assert.commandFailed(db.adminCommand({moveChunk: db.getName() + ".unsharded", find: {_id: 1}, to: shardNames[1]}));
}

function testHashed(db) {
    jsTestLog("Test moveChunk on sharded collection with hashed key using find and bounds.");
    const collName = jsTestName() + "_hashed";
    const ns = db.getName() + "." + collName;
    const coll = db.getCollection(collName);

    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: "hashed"}, numInitialChunks: 1}));

    let aChunk;
    let shard0;
    let shard1;
    [aChunk, shard0, shard1] = getInitialChunkAndShardNames(configDB, ns, shardNames);
    assert(aChunk);

    // Error if either of the bounds is not a valid shard key (BSON object - 1 yields a NaN)
    assert.commandFailed(db.adminCommand({moveChunk: ns, bounds: [MinKey - 1, MaxKey], to: shard1}));
    assert.commandFailed(db.adminCommand({moveChunk: ns, bounds: [MinKey, MaxKey - 1], to: shard1}));

    // Fail if find and bounds are both set.
    assert.commandFailed(db.adminCommand({moveChunk: ns, find: {_id: 1}, bounds: [MinKey, MaxKey], to: shard1}));

    assert.commandWorked(db.adminCommand({moveChunk: ns, bounds: [aChunk.min, aChunk.max], to: shard1}));

    assert.eq(0, configDB.chunks.count({_id: aChunk._id, shard: shard0}));
    assert.eq(1, configDB.chunks.count({_id: aChunk._id, shard: shard1}));

    coll.drop();
}

function testNotHashed(db, keyDoc) {
    jsTestLog("Test moveChunk on sharded collection with key " + tojson(keyDoc) + " using find and bounds.");
    const collName = jsTestName();
    const ns = db.getName() + "." + collName;
    const coll = db.getCollection(collName);

    // Fail if find is not a valid shard key.
    const res = db.adminCommand({shardCollection: ns, key: keyDoc});
    // Some suites install an index that conflicts with the shard key, so skip this test case.
    if (!res.ok && res.code == ErrorCodes.AlreadyInitialized) {
        jsTest.log("Skipping testNotHashed with " + tojson(keyDoc) + " because " + res.errmsg);
        coll.drop();
        return;
    }

    let chunk;
    let shard0;
    let shard1;
    [chunk, shard0, shard1] = getInitialChunkAndShardNames(configDB, ns, shardNames);
    const chunkId = chunk._id;

    assert.commandFailed(db.adminCommand({moveChunk: ns, find: {xxx: 1}, to: shard1}));
    assert.eq(shard0, configDB.chunks.findOne({_id: chunkId}).shard);

    assert.commandWorked(db.adminCommand({moveChunk: ns, find: keyDoc, to: shard1}));
    assert.eq(shard1, configDB.chunks.findOne({_id: chunkId}).shard);

    // Fail if to shard that does not exist.
    assert.commandFailed(db.adminCommand({moveChunk: ns, find: keyDoc, to: "WrongShard"}));

    // Fail if chunk is already at shard.
    assert.eq(shard1, configDB.chunks.findOne({_id: chunkId}).shard);

    coll.drop();
}

print(jsTestName() + " is running on " + shardNames.length + " shards.");

const mongos = db.getMongo();
const dbName = jsTestName() + "db";
const db2 = mongos.getDB(dbName);

assert.commandWorked(db2.adminCommand({enableSharding: dbName, primaryShard: shardNames[0]}));

testErrorOnInvalidNamespace(db2);
testErrorOnDatabaseDNE(db2);
testErrorOnUnshardedCollection(db2);
testHashed(db2);
testNotHashed(db2, {a: 1});
testNotHashed(db2, {a: 1, b: 1});

db2.dropDatabase();
