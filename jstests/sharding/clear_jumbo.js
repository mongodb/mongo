/**
 * Use the 'requires_find_command' tag to skip this test in sharding_op_query suite. Otherwise,
 * sessionDB.coll.find() will throw "Cannot run a legacy query on a session".
 *
 * @tags: [
 *  requires_find_command,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");

let st = new ShardingTest({shards: 2});

const mongosSession = st.s.startSession({retryWrites: true});
const sessionAdminDB = mongosSession.getDatabase('admin');
const sessionConfigDB = mongosSession.getDatabase('config');

assert.commandWorked(sessionAdminDB.runCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(
    sessionAdminDB.runCommand({addShardToZone: st.shard1.shardName, zone: 'finalDestination'}));

////////////////////////////////////////////////////////////////////////////
// Ranged shard key
assert.commandWorked(sessionAdminDB.runCommand({shardCollection: 'test.range', key: {x: 1}}));
assert.commandWorked(sessionAdminDB.runCommand({split: 'test.range', middle: {x: 0}}));

let chunkColl = sessionConfigDB.chunks;

let testRangeColl = sessionConfigDB.collections.findOne({_id: 'test.range'});
if (testRangeColl.timestamp) {
    assert.commandWorked(
        chunkColl.update({uuid: testRangeColl.uuid, min: {x: 0}}, {$set: {jumbo: true}}));
} else {
    assert.commandWorked(chunkColl.update({ns: 'test.range', min: {x: 0}}, {$set: {jumbo: true}}));
}

let jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
let jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
assert.commandWorked(sessionAdminDB.runCommand({clearJumboFlag: 'test.range', find: {x: -1}}));
jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(sessionAdminDB.runCommand({clearJumboFlag: 'test.range', find: {x: 1}}));
jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

////////////////////////////////////////////////////////////////////////////
// Hashed shard key
assert.commandWorked(sessionAdminDB.runCommand(
    {shardCollection: 'test.hashed', key: {x: 'hashed'}, numInitialChunks: 2}));

let testHashedColl = sessionConfigDB.collections.findOne({_id: 'test.hashed'});
if (testHashedColl.timestamp) {
    assert.commandWorked(
        chunkColl.update({uuid: testHashedColl.uuid, min: {x: 0}}, {$set: {jumbo: true}}));
} else {
    assert.commandWorked(chunkColl.update({ns: 'test.hashed', min: {x: 0}}, {$set: {jumbo: true}}));
}
jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.hashed', {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
let unrelatedChunk =
    findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.hashed', {min: {x: MinKey}});
assert.commandWorked(sessionAdminDB.runCommand(
    {clearJumboFlag: 'test.hashed', bounds: [unrelatedChunk.min, unrelatedChunk.max]}));
jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.hashed', {min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(sessionAdminDB.runCommand(
    {clearJumboFlag: 'test.hashed', bounds: [jumboChunk.min, jumboChunk.max]}));
jumboChunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.hashed', {min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

////////////////////////////////////////////////////////////////////////////
// Ensure clear jumbo flag stores the correct chunk version

let version = st.configRS.getPrimary()
                  .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                  .featureCompatibilityVersion.version;
if (version === '5.0') {
    assert.eq(undefined, jumboChunk.lastmodEpoch);
    assert.eq(undefined, jumboChunk.lastmodTimestamp);
}

////////////////////////////////////////////////////////////////////////////
// Balancer with jumbo chunks behavior
// Forces a jumbo chunk to be on a wrong zone but balancer shouldn't be able to move it until
// jumbo flag is cleared.

st.stopBalancer();

if (testRangeColl.timestamp) {
    assert.commandWorked(
        chunkColl.update({uuid: testRangeColl.uuid, min: {x: 0}}, {$set: {jumbo: true}}));
} else {
    assert.commandWorked(chunkColl.update({ns: 'test.range', min: {x: 0}}, {$set: {jumbo: true}}));
}
assert.commandWorked(sessionAdminDB.runCommand(
    {updateZoneKeyRange: 'test.range', min: {x: 0}, max: {x: MaxKey}, zone: 'finalDestination'}));

let chunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert(chunk.jumbo, tojson(chunk));
assert.eq(st.shard0.shardName, chunk.shard);

st._configServers.forEach((conn) => {
    conn.adminCommand({
        configureFailPoint: 'overrideBalanceRoundInterval',
        mode: 'alwaysOn',
        data: {intervalMs: 200}
    });
});

let waitForBalancerToRun = function() {
    let lastRoundNumber =
        assert.commandWorked(sessionAdminDB.runCommand({balancerStatus: 1})).numBalancerRounds;
    st.startBalancer();

    assert.soon(function() {
        let res = assert.commandWorked(sessionAdminDB.runCommand({balancerStatus: 1}));
        return res.mode == "full" && res.numBalancerRounds - lastRoundNumber > 1;
    });

    st.stopBalancer();
};

waitForBalancerToRun();

chunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert.eq(st.shard0.shardName, chunk.shard);

assert.commandWorked(sessionAdminDB.runCommand({clearJumboFlag: 'test.range', find: {x: 0}}));

waitForBalancerToRun();

chunk = findChunksUtil.findOneChunkByNs(sessionConfigDB, 'test.range', {min: {x: 0}});
assert.eq(st.shard1.shardName, chunk.shard);

st.stop();
})();
