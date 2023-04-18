// requires_fcv_61 since the balancer in v6.0 is still working on the number of chunks,
// hence the balancer is not triggered and the chunk is not marked as jumbo
// @tags: [requires_fcv_61]

// Cannot run the filtering metadata check on tests that run clearJumboFlag.
TestData.skipCheckShardFilteringMetadata = true;

(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/libs/fail_point_util.js");

let st = new ShardingTest({shards: 2, other: {chunkSize: 1}});

const mongosSession = st.s.startSession({retryWrites: true});
const adminDB = mongosSession.getDatabase('admin');
const configDB = mongosSession.getDatabase('config');
const testDB = mongosSession.getDatabase('test');
const testColl = testDB.getCollection('range');
const hashedTestColl = testDB.getCollection('hashed');

function runBalancer(coll) {
    st.startBalancer();

    // Let the balancer run until balanced.
    st.printShardingStatus(true);
    st.awaitBalance(coll.getName(), coll.getDB());
    st.printShardingStatus(true);

    st.stopBalancer();
}

function createJumboChunk(coll, keyValue) {
    const largeString = 'X'.repeat(1024 * 1024);  // 1 MB

    // Create sufficient documents to create a jumbo chunk, and use the same shard key in all of
    // them so that the chunk cannot be split.
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 10; i++) {
        bulk.insert({x: keyValue, big: largeString, i: i});
    }
    assert.commandWorked(bulk.execute());
    runBalancer(coll);
}

function validateJumboFlag(ns, query) {
    let jumboChunk = findChunksUtil.findOneChunkByNs(configDB, ns, query);
    assert.eq(jumboChunk.jumbo, true);
}

// Initializing test database
assert.commandWorked(
    adminDB.runCommand({enableSharding: 'test', primaryShard: st.shard0.shardName}));
assert.commandWorked(adminDB.runCommand({addShardToZone: st.shard1.shardName, zone: 'ZoneShard1'}));

////////////////////////////////////////////////////////////////////////////
// Ranged shard key
let testNs = testColl.getFullName();

assert.commandWorked(adminDB.runCommand({shardCollection: testNs, key: {x: 1}}));
assert.commandWorked(adminDB.runCommand({split: testNs, middle: {x: 0}}));

createJumboChunk(testColl, 0);
validateJumboFlag(testNs, {min: {x: 0}});
let jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
let jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
assert.commandWorked(adminDB.runCommand({clearJumboFlag: testNs, find: {x: -1}}));
jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(adminDB.runCommand({clearJumboFlag: testNs, find: {x: 1}}));
jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Delete all documents
assert.commandWorked(testColl.deleteMany({x: 0}));
let docCount = assert.commandWorked(adminDB.runCommand({count: testNs}));
assert.eq(docCount.n, 0);

////////////////////////////////////////////////////////////////////////////
// Hashed shard key
testNs = hashedTestColl.getFullName();

assert.commandWorked(
    adminDB.runCommand({shardCollection: testNs, key: {x: 'hashed'}, numInitialChunks: 2}));

createJumboChunk(hashedTestColl, 0);
validateJumboFlag(testNs, {min: {x: 0}});

jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
let unrelatedChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: MinKey}});
assert.commandWorked(
    adminDB.runCommand({clearJumboFlag: testNs, bounds: [unrelatedChunk.min, unrelatedChunk.max]}));
jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(
    adminDB.runCommand({clearJumboFlag: testNs, bounds: [jumboChunk.min, jumboChunk.max]}));
jumboChunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Ensure clear jumbo flag stores the correct chunk version
assert.eq(undefined, jumboChunk.lastmodEpoch);
assert.eq(undefined, jumboChunk.lastmodTimestamp);

// Delete all documents
assert.commandWorked(hashedTestColl.deleteMany({x: 0}));
docCount = assert.commandWorked(adminDB.runCommand({count: testNs}));
assert.eq(docCount.n, 0);

////////////////////////////////////////////////////////////////////////////
// Balancer with jumbo chunks behavior
// Forces a jumbo chunk to be on a wrong zone but balancer shouldn't be able to move it until
// jumbo flag is cleared.
testNs = testColl.getFullName();

st.stopBalancer();

assert.commandWorked(adminDB.runCommand(
    {updateZoneKeyRange: testNs, min: {x: 0}, max: {x: MaxKey}, zone: 'ZoneShard1'}));

createJumboChunk(testColl, 0);
validateJumboFlag(testNs, {min: {x: 0}});

let chunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(chunk.jumbo, tojson(chunk));
assert.eq(st.shard0.shardName, chunk.shard);

configureFailPointForRS(
    st.configRS.nodes, 'overrideBalanceRoundInterval', {intervalMs: 200}, 'alwaysOn');

runBalancer(testColl);

// Verify chunk stays in shard0
chunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert.eq(st.shard0.shardName, chunk.shard);

// Delete all documents
assert.commandWorked(testColl.deleteMany({x: 0}));
docCount = assert.commandWorked(adminDB.runCommand({count: testNs}));
assert.eq(docCount.n, 0);

// Clear jumbo flag
assert.commandWorked(adminDB.runCommand({clearJumboFlag: testNs, find: {x: 0}}));
chunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));

runBalancer(testColl);

// Verify chunk is moved to shard1
chunk = findChunksUtil.findOneChunkByNs(configDB, testNs, {min: {x: 0}});
assert.eq(st.shard1.shardName, chunk.shard);

st.stop();
})();
