/**
 * Tests that resharding can complete successfully when the original collection has a large number
 * of chunks.
 *
 * @tags: [
 *   uses_atclustertime,
 *   resource_intensive,
 *   exclude_from_large_txns
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

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

let nZones = 10000;
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
