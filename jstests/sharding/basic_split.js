/**
 * Perform basic tests for the split command against mongos.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({mongos: 2, shards: 2, other: {chunkSize: 1}});
let configDB = st.s0.getDB("config");

let shard0 = st.shard0.shardName;
let shard1 = st.shard1.shardName;

// split on invalid ns.
assert.commandFailed(configDB.adminCommand({split: "user", key: {_id: 1}}));

// split on unsharded collection (db is not sharding enabled).
assert.commandFailed(configDB.adminCommand({split: "test.user", key: {_id: 1}}));

assert.commandWorked(configDB.adminCommand({enableSharding: "test", primaryShard: shard0}));

// split on unsharded collection (db is sharding enabled).
assert.commandFailed(configDB.adminCommand({split: "test.user", key: {_id: 1}}));

assert.commandWorked(configDB.adminCommand({shardCollection: "test.user", key: {_id: 1}}));

assert.eq(null, findChunksUtil.findOneChunkByNs(configDB, "test.user", {min: {_id: 0}}));

assert.commandWorked(configDB.adminCommand({split: "test.user", middle: {_id: 0}}));
assert.neq(null, findChunksUtil.findOneChunkByNs(configDB, "test.user", {min: {_id: 0}}));

// TODO(SERVER-97588): Remove version check from tests when 8.1 becomes last LTS.
const fcvDoc = configDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0) {
    // It should not fail on boundaries that have already been split.
    assert.commandWorked(configDB.adminCommand({split: "test.user", middle: {_id: 0}}));
} else {
    // Cannot split on existing chunk boundary.
    assert.commandFailed(configDB.adminCommand({split: "test.user", middle: {_id: 0}}));
}

// Attempt to split on a value that is not the shard key.
assert.commandFailed(configDB.adminCommand({split: "test.user", middle: {x: 100}}));
assert.commandFailed(configDB.adminCommand({split: "test.user", find: {x: 100}}));
assert.commandFailed(configDB.adminCommand({split: "test.user", bounds: [{x: MinKey}, {x: MaxKey}]}));

// Insert documents large enough to fill up a chunk
let kiloDoc = "x".repeat(1023);
let testDB = st.s.getDB("test");
let bulk = testDB.user.initializeUnorderedBulkOp();
for (var x = -1200; x < 1200; x++) {
    bulk.insert({_id: x, val: kiloDoc});
}
assert.commandWorked(bulk.execute());

assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.user", {min: {$gte: {_id: 0}}}).itcount());

// Errors if bounds do not correspond to existing chunk boundaries.
assert.commandFailed(configDB.adminCommand({split: "test.user", bounds: [{_id: 0}, {_id: 1000}]}));
assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.user", {min: {$gte: {_id: 0}}}).itcount());

assert.commandWorked(configDB.adminCommand({split: "test.user", bounds: [{_id: 0}, {_id: MaxKey}]}));
assert.gt(findChunksUtil.findChunksByNs(configDB, "test.user", {min: {$gte: {_id: 0}}}).itcount(), 1);

assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.user", {min: {$lt: {_id: 0}}}).itcount());
assert.commandWorked(configDB.adminCommand({split: "test.user", middle: {_id: -600}}));
assert.gt(findChunksUtil.findChunksByNs(configDB, "test.user", {min: {$lt: {_id: 0}}}).itcount(), 1);

// Mongos must refresh metadata if the chunk version does not match
assert.commandWorked(st.s0.adminCommand({moveChunk: "test.user", find: {_id: -900}, to: shard1, _waitForDelete: true}));
assert.commandWorked(st.s1.adminCommand({split: "test.user", middle: {_id: -900}}));
assert.commandWorked(st.s1.adminCommand({moveChunk: "test.user", find: {_id: -900}, to: shard0, _waitForDelete: true}));
assert.commandWorked(st.s1.adminCommand({moveChunk: "test.user", find: {_id: -901}, to: shard0, _waitForDelete: true}));
assert.eq(0, findChunksUtil.findChunksByNs(configDB, "test.user", {shard: shard1}).itcount());

//
// Compound Key
//

assert.commandWorked(configDB.adminCommand({shardCollection: "test.compound", key: {x: 1, y: 1}}));

assert.eq(null, findChunksUtil.findOneChunkByNs(configDB, "test.compound", {min: {x: 0, y: 0}}));
assert.commandWorked(configDB.adminCommand({split: "test.compound", middle: {x: 0, y: 0}}));
assert.neq(null, findChunksUtil.findOneChunkByNs(configDB, "test.compound", {min: {x: 0, y: 0}}));

// TODO(SERVER-97588): Remove version check from tests when 8.1 becomes last LTS.
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0) {
    // It should not fail on boundaries that have already been split.
    assert.commandWorked(configDB.adminCommand({split: "test.compound", middle: {x: 0, y: 0}}));
    assert.commandWorked(configDB.adminCommand({split: "test.compound", middle: {x: MinKey, y: MinKey}}));
    assert.commandWorked(configDB.adminCommand({split: "test.compound", middle: {x: MaxKey, y: MaxKey}}));
} else {
    // cannot split on existing chunk boundary.
    assert.commandFailed(configDB.adminCommand({split: "test.compound", middle: {x: 0, y: 0}}));
    assert.commandFailed(configDB.adminCommand({split: "test.compound", middle: {x: MinKey, y: MinKey}}));
    assert.commandFailed(configDB.adminCommand({split: "test.compound", middle: {x: MaxKey, y: MaxKey}}));
}

bulk = testDB.compound.initializeUnorderedBulkOp();
for (x = -1200; x < 1200; x++) {
    bulk.insert({x: x, y: x, val: kiloDoc});
}
assert.commandWorked(bulk.execute());

assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.compound", {min: {$gte: {x: 0, y: 0}}}).itcount());
assert.commandWorked(
    configDB.adminCommand({
        split: "test.compound",
        bounds: [
            {x: 0, y: 0},
            {x: MaxKey, y: MaxKey},
        ],
    }),
);
assert.gt(findChunksUtil.findChunksByNs(configDB, "test.compound", {min: {$gte: {x: 0, y: 0}}}).itcount(), 1);

assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.compound", {min: {$lt: {x: 0, y: 0}}}).itcount());
assert.commandWorked(configDB.adminCommand({split: "test.compound", find: {x: -1, y: -1}}));
assert.gt(findChunksUtil.findChunksByNs(configDB, "test.compound", {min: {$lt: {x: 0, y: 0}}}).itcount(), 1);

st.stop();
