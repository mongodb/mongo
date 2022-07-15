// Tests whether the noBalance flag disables balancing for collections
// @tags: [requires_sharding]

(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/libs/feature_flag_util.js");

const st = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableAutoSplit: false}});

// First, test that shell helpers require an argument
assert.throws(sh.disableBalancing, [], "sh.disableBalancing requires a collection");
assert.throws(sh.enableBalancing, [], "sh.enableBalancing requires a collection");

var shardAName = st.shard0.shardName;
var shardBName = st.shard1.shardName;

const dbName = jsTest.name();
const collAName = 'collA';
const collBName = 'collB';
const collA = st.s.getCollection(dbName + '.' + collAName);
const collB = st.s.getCollection(dbName + '.' + collBName);

// Shard two collections
st.shardColl(collA, {_id: 1}, false);
st.shardColl(collB, {_id: 1}, false);

// Disable balancing on one collection
sh.disableBalancing(collB);

// Insert 10MB data so balancing can occur
const bigString = 'X'.repeat(1024 * 1024);  // 1MB
const bulkA = collA.initializeUnorderedBulkOp();
var bulkB = collB.initializeUnorderedBulkOp();
for (var i = 0; i < 10; i++) {
    bulkA.insert({_id: i, s: bigString});
    assert.commandWorked(st.s.adminCommand({split: collA.getFullName(), middle: {_id: i}}));
    bulkB.insert({_id: i, s: bigString});
    assert.commandWorked(st.s.adminCommand({split: collB.getFullName(), middle: {_id: i}}));
}
assert.commandWorked(bulkA.execute());
assert.commandWorked(bulkB.execute());

jsTest.log("Balancing disabled on " + collB);
printjson(collA.getDB().getSiblingDB("config").collections.find().toArray());

st.startBalancer();

// Make sure collA gets balanced
st.awaitBalance(collAName, dbName, 60 * 1000);

jsTest.log("Chunks for " + collA + " are balanced.");

// Check that the collB chunks were not moved
var shardAChunks =
    findChunksUtil.findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardAName})
        .itcount();
var shardBChunks =
    findChunksUtil.findChunksByNs(st.s.getDB("config"), collB.getFullName(), {shard: shardBName})
        .itcount();
printjson({shardA: shardAChunks, shardB: shardBChunks});
assert(shardAChunks == 0 || shardBChunks == 0);

// Re-enable balancing for collB
sh.enableBalancing(collB);

// Make sure that collB is now balanced
st.awaitBalance(collBName, dbName, 60 * 1000);

jsTest.log("Chunks for " + collB + " are balanced.");

// Re-disable balancing for collB
sh.disableBalancing(collB);

// Wait for the balancer to fully finish the last migration and write the changelog
// MUST set db var here, ugly but necessary
db = st.s0.getDB("config");
st.awaitBalancerRound();

// Make sure auto-migrates on insert don't move data
var lastMigration = sh._lastMigration(collB);

bulkB = collB.initializeUnorderedBulkOp();
for (var i = 10; i < 20; i++) {
    bulkB.insert({_id: i, s: bigString});
}
assert.commandWorked(bulkB.execute());

printjson(lastMigration);
printjson(sh._lastMigration(collB));

if (lastMigration == null) {
    assert.eq(null, sh._lastMigration(collB));
} else {
    assert.eq(lastMigration.time, sh._lastMigration(collB).time);
}

st.stop();
}());
