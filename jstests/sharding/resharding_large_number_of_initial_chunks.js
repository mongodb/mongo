/**
 * Tests that resharding can complete successfully when the original collection has a large number
 * of chunks.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 *   resource_intensive
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");

// The total size of the zones and chunks documents that the config server writes in a single
// replica set transaction totals to around 60 MB. WWhen combined with the other operations and
// transactions occurring in the config server, this large transaction causes WiredTiger to run out
// of dirty cache space. Hence, we need to increase the wiredTigerCacheSizeGB to 5 GB.
const st = new ShardingTest(
    {mongos: 1, shards: 2, config: 1, other: {configOptions: {wiredTigerCacheSizeGB: 5}}});

const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// In debug builds, the resharding operation takes significantly longer when there's a large number
// of chunks and zones. We reduce nZones so the test completes in a reasonable amount of time.
let nZones = buildInfo().debug ? 10000 : 175000;
let zones = [];
let shard0Zones = [];
let shard1Zones = [];
for (let i = 0; i < nZones; i++) {
    let zoneName = "zone" + i;
    zones.push({zone: zoneName, min: {"newKey": i}, max: {"newKey": i + 1}});

    if (i % 2 == 0) {
        shard0Zones.push(zoneName);
    } else {
        shard1Zones.push(zoneName);
    }
}

jsTestLog("Updating First Zone");
assert.commandWorked(
    mongos.getDB("config").shards.update({_id: st.shard0.shardName}, {$set: {tags: shard0Zones}}));
jsTestLog("Updating First Zone");
assert.commandWorked(
    mongos.getDB("config").shards.update({_id: st.shard1.shardName}, {$set: {tags: shard1Zones}}));

jsTestLog("Resharding Collection");
assert.commandWorked(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, zones: zones}));

// Assert that the correct number of zones and chunks documents exist after resharding 'db.foo'.
// There should be two more chunks docs than zones docs created to cover the ranges
// {newKey: minKey -> newKey : 0} and {newKey: nZones -> newKey : maxKey} which are not associated
// with a zone.
assert.eq(mongos.getDB("config").tags.find({ns: ns}).itcount(), nZones);
assert.eq(findChunksUtil.countChunksForNs(mongos.getDB("config"), ns), nZones + 2);

st.stop();
})();
