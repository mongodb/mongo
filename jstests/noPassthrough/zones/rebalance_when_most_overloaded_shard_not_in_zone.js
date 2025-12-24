/**
 * Insert 20MB of docs in EU zone (shard 0) and 10MB in US zone (shards 1 and 2), then make sure US zone gets properly balanced (configured "balancing unit" 1MB).
 * This is a regression test for SERVER-115962.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 3,
    rs: {nodes: 1},
    config: 1,
    mongos: 1,
    other: {chunkSize: 1, enableBalancer: false},
});

const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;

const fullNs = "test.coll";

// Assign zones: shard0 'EU' - shard1, shard2 'US', then tag `test.coll` before sharding it
assert.commandWorked(mongos.adminCommand({addShardToZone: shard0, zone: "EU"}));
assert.commandWorked(mongos.adminCommand({addShardToZone: shard1, zone: "US"}));
assert.commandWorked(mongos.adminCommand({addShardToZone: shard2, zone: "US"}));
assert.commandWorked(
    mongos.adminCommand({
        updateZoneKeyRange: fullNs,
        min: {"region": "EU", counter: MinKey()},
        max: {"region": "US", counter: MinKey()},
        zone: "EU",
    }),
);
assert.commandWorked(
    mongos.adminCommand({
        updateZoneKeyRange: fullNs,
        min: {"region": "US", counter: MinKey()},
        max: {"region": "UT", counter: MinKey()},
        zone: "US",
    }),
);

assert.commandWorked(mongos.adminCommand({shardCollection: fullNs, key: {region: 1, counter: 1}}));

// Insert 20MB of docs in EU zone and 10MB in US zone
const string1MB = "A".repeat(1024 * 1024);
var EUdocs = [];
for (var i = 0; i < 20; i++) {
    EUdocs.push({region: "EU", counter: i, str: string1MB});
}
assert.commandWorked(mongos.getCollection(fullNs).insertMany(EUdocs));

var USdocs = [];
for (var i = 0; i < 10; i++) {
    USdocs.push({region: "US", counter: i, str: string1MB});
}
assert.commandWorked(mongos.getCollection(fullNs).insertMany(USdocs));

// Start the balancer and verify that the US zone gets properly balanced
assert.commandWorked(st.startBalancer());

const expectedDistribution = {};
expectedDistribution[shard0] = 20;
expectedDistribution[shard1] = 6;
expectedDistribution[shard2] = 4;

assert.soonNoExcept(() => {
    const dataDistribution = mongos
        .getDB("admin")
        .aggregate([{$shardedDataDistribution: {}}, {$match: {ns: fullNs}}])
        .next();
    dataDistribution.shards.forEach((shard) =>
        assert.eq(expectedDistribution[shard.shardName], shard.numOwnedDocuments),
    );
    return true;
});

st.stop();
