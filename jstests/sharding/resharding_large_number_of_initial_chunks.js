/**
 * Tests that resharding can complete successfully when it has a large number
 * of chunks being created during the process.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    config: 1,
    other: {
        configOptions: {
            setParameter:
                {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000 /* 1 day */}
        }
    }
});

const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

let nChunks = 100000;
let newChunks = [];

newChunks.push({min: {newKey: MinKey}, max: {newKey: 0}, recipientShardId: shard0});
for (let i = 0; i < nChunks; i++) {
    if (i % 2 == 0) {
        newChunks.push({min: {newKey: i}, max: {newKey: i + 1}, recipientShardId: shard0});
    } else {
        newChunks.push({min: {newKey: i}, max: {newKey: i + 1}, recipientShardId: shard1});
    }
}
newChunks.push({min: {newKey: nChunks}, max: {newKey: MaxKey}, recipientShardId: shard1});

jsTestLog("Resharding Collection");
assert.commandWorked(mongos.adminCommand(
    {reshardCollection: ns, key: {newKey: 1}, _presetReshardedChunks: newChunks}));

// Assert that the correct number of chunks documents exist after resharding 'db.foo'.
// There should be two more chunks docs to cover the ranges
// {newKey: minKey -> newKey : 0} and {newKey: nChunks -> newKey : maxKey}
assert.eq(findChunksUtil.countChunksForNs(mongos.getDB("config"), ns), nChunks + 2);

st.stop();
})();
