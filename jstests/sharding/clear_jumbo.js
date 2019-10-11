(function() {
"use strict";

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'finalDestination'}));

////////////////////////////////////////////////////////////////////////////
// Ranged shard key
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.range', key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'test.range', middle: {x: 0}}));

let chunkColl = st.s.getDB('config').chunks;
assert.commandWorked(chunkColl.update({ns: 'test.range', min: {x: 0}}, {$set: {jumbo: true}}));

let jumboChunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
let jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
assert.commandWorked(st.s.adminCommand({clearJumboFlag: 'test.range', find: {x: -1}}));
jumboChunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(st.s.adminCommand({clearJumboFlag: 'test.range', find: {x: 1}}));
jumboChunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

////////////////////////////////////////////////////////////////////////////
// Hashed shard key
assert.commandWorked(
    st.s.adminCommand({shardCollection: 'test.hashed', key: {x: 'hashed'}, numInitialChunks: 2}));
assert.commandWorked(chunkColl.update({ns: 'test.hashed', min: {x: 0}}, {$set: {jumbo: true}}));
jumboChunk = chunkColl.findOne({ns: 'test.hashed', min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
jumboMajorVersionBefore = jumboChunk.lastmod.getTime();

// Target non-jumbo chunk should not affect real jumbo chunk.
let unrelatedChunk = chunkColl.findOne({ns: 'test.hashed', min: {x: MinKey}});
assert.commandWorked(st.s.adminCommand(
    {clearJumboFlag: 'test.hashed', bounds: [unrelatedChunk.min, unrelatedChunk.max]}));
jumboChunk = chunkColl.findOne({ns: 'test.hashed', min: {x: 0}});
assert(jumboChunk.jumbo, tojson(jumboChunk));
assert.eq(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

// Target real jumbo chunk should bump version.
assert.commandWorked(
    st.s.adminCommand({clearJumboFlag: 'test.hashed', bounds: [jumboChunk.min, jumboChunk.max]}));
jumboChunk = chunkColl.findOne({ns: 'test.hashed', min: {x: 0}});
assert(!jumboChunk.jumbo, tojson(jumboChunk));
assert.lt(jumboMajorVersionBefore, jumboChunk.lastmod.getTime());

////////////////////////////////////////////////////////////////////////////
// Balancer with jumbo chunks behavior
// Forces a jumbo chunk to be on a wrong zone but balancer shouldn't be able to move it until
// jumbo flag is cleared.

st.stopBalancer();
assert.commandWorked(chunkColl.update({ns: 'test.range', min: {x: 0}}, {$set: {jumbo: true}}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: 'test.range', min: {x: 0}, max: {x: MaxKey}, zone: 'finalDestination'}));

let chunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
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
        assert.commandWorked(st.s.adminCommand({balancerStatus: 1})).numBalancerRounds;
    st.startBalancer();

    assert.soon(function() {
        let roundNumber =
            assert.commandWorked(st.s.adminCommand({balancerStatus: 1})).numBalancerRounds;
        return roundNumber > lastRoundNumber;
    });

    st.stopBalancer();
};

waitForBalancerToRun();

chunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
assert.eq(st.shard0.shardName, chunk.shard);

assert.commandWorked(st.s.adminCommand({clearJumboFlag: 'test.range', find: {x: 0}}));

waitForBalancerToRun();

chunk = chunkColl.findOne({ns: 'test.range', min: {x: 0}});
assert.eq(st.shard1.shardName, chunk.shard);

st.stop();
})();
