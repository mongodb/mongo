/**
 * Test that chunks and documents are moved correctly after zone changes.
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");
load("jstests/sharding/libs/zone_changes_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

/**
 * Adds each shard to the corresponding zone in zoneTags, and makes the zone range equal
 * to the chunk range of the shard. Assumes that there are no chunk holes on each shard.
 */
function addShardsToZonesAndAssignZoneRanges(st, ns, shardChunkBounds, shardTags) {
    let zoneChunks = {};
    for (let [shardName, chunkBounds] of Object.entries(shardChunkBounds)) {
        let zoneName = shardTags[shardName][0];
        let rangeMin = {x: MaxKey};
        let rangeMax = {x: MinKey};
        for (let bounds of chunkBounds) {
            if (chunkBoundsUtil.lt(bounds[0], rangeMin)) {
                rangeMin = bounds[0];
            }
            if (chunkBoundsUtil.gte(bounds[1], rangeMax)) {
                rangeMax = bounds[1];
            }
        }
        zoneChunks[zoneName] = chunkBounds;
        assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zoneName}));
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: rangeMin, max: rangeMax, zone: zoneName}));
    }
    return zoneChunks;
}

/**
 * Returns the highest chunk bounds out of the given chunk bounds. Assumes that the
 * chunks do not overlap.
 */
function findHighestChunkBounds(chunkBounds) {
    let highestBounds = chunkBounds[0];
    for (let i = 1; i < chunkBounds.length; i++) {
        if (chunkBoundsUtil.lt(highestBounds, chunkBounds[i])) {
            highestBounds = chunkBounds[i];
        }
    }
    return highestBounds;
}

const st = new ShardingTest({shards: 3, other: {chunkSize: 1, enableAutoSplitter: false}});
let primaryShard = st.shard0;
let dbName = "test";
let testDB = st.s.getDB(dbName);
let configDB = st.s.getDB("config");
let coll = testDB.hashed;
let ns = coll.getFullName();
let shardKey = {x: "hashed"};

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}));

jsTest.log(
    "Shard the collection. The command creates two chunks on each of the shards by default.");
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey}));
let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).sort({min: 1}).toArray();
let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

jsTest.log("Insert docs (one for each chunk) and check that they end up on the right shards.");
const bigString = 'X'.repeat(1024 * 1024);  // 1MB
let docs = [
    {x: -25, s: bigString},
    {x: -18, s: bigString},
    {x: -5, s: bigString},
    {x: -1, s: bigString},
    {x: 5, s: bigString},
    {x: 10, s: bigString}
];
assert.commandWorked(coll.insert(docs));

let docChunkBounds = [];
let minHash = MaxKey;
docs.forEach(function(doc) {
    let hash = convertShardKeyToHashed(doc.x);
    let {shard, bounds} =
        chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, {x: hash});
    assert.eq(1, shard.getCollection(ns).count(doc));
    docChunkBounds.push(bounds);
    if (bsonWoCompare(hash, minHash) < 0) {
        minHash = hash;
    }
});
assert.eq(docs.length, (new Set(docChunkBounds)).size);
assert.eq(docs.length, findChunksUtil.countChunksForNs(configDB, ns));

jsTest.log(
    "Assign each shard a zone, make each zone range equal to the chunk range for the shard, " +
    "and store the chunks for each zone.");
let shardTags = {
    [st.shard0.shardName]: ["zoneA"],
    [st.shard1.shardName]: ["zoneB"],
    [st.shard2.shardName]: ["zoneC"]
};
let zoneChunkBounds = addShardsToZonesAndAssignZoneRanges(st, ns, shardChunkBounds, shardTags);
assertShardTags(configDB, shardTags);

jsTest.log("Test shard's zone changes...");

jsTest.log(
    "Check that removing a zone from a shard causes its chunks and documents to move to other" +
    " shards that the zone belongs to.");
moveZoneToShard(st, "zoneA", st.shard0, st.shard1);
shardTags = {
    [st.shard0.shardName]: [],
    [st.shard1.shardName]: ["zoneB", "zoneA"],
    [st.shard2.shardName]: ["zoneC"]
};
assertShardTags(configDB, shardTags);

runBalancer(st, zoneChunkBounds["zoneA"].length);
shardChunkBounds = {
    [st.shard0.shardName]: [],
    [st.shard1.shardName]: [...zoneChunkBounds["zoneB"], ...zoneChunkBounds["zoneA"]],
    [st.shard2.shardName]: zoneChunkBounds["zoneC"]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Check that the balancer balances chunks within zones.");
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zoneB"}));
shardTags = {
    [st.shard0.shardName]: ["zoneB"],
    [st.shard1.shardName]: ["zoneB", "zoneA"],
    [st.shard2.shardName]: ["zoneC"]
};
assertShardTags(configDB, shardTags);

const balanceAccordingToDataSize = FeatureFlagUtil.isEnabled(
    st.configRS.getPrimary().getDB('admin'), "BalanceAccordingToDataSize");
let numChunksToMove = balanceAccordingToDataSize ? zoneChunkBounds["zoneB"].length - 1
                                                 : zoneChunkBounds["zoneB"].length / 2;
runBalancer(st, numChunksToMove);
shardChunkBounds = {
    [st.shard0.shardName]: zoneChunkBounds["zoneB"].slice(0, numChunksToMove),
    [st.shard1.shardName]: [
        ...zoneChunkBounds["zoneA"],
        ...zoneChunkBounds["zoneB"].slice(numChunksToMove, zoneChunkBounds["zoneB"].length)
    ],
    [st.shard2.shardName]: zoneChunkBounds["zoneC"]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Make another zone change, and check that the chunks and docs are on the right shards.");
assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "zoneB"}));
moveZoneToShard(st, "zoneC", st.shard2, st.shard0);
moveZoneToShard(st, "zoneA", st.shard1, st.shard2);
shardTags = {
    [st.shard0.shardName]: ["zoneC"],
    [st.shard1.shardName]: ["zoneB"],
    [st.shard2.shardName]: ["zoneA"]
};
assertShardTags(configDB, shardTags);

runBalancer(st,
            numChunksToMove + zoneChunkBounds["zoneA"].length + zoneChunkBounds["zoneC"].length);
shardChunkBounds = {
    [st.shard0.shardName]: zoneChunkBounds["zoneC"],
    [st.shard1.shardName]: zoneChunkBounds["zoneB"],
    [st.shard2.shardName]: zoneChunkBounds["zoneA"]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Test chunk's zone changes...");

// Find the chunk with the highest bounds in zoneA.
let originalZoneARange = chunkBoundsUtil.computeRange(zoneChunkBounds["zoneA"]);
let targetChunkBounds = findHighestChunkBounds(zoneChunkBounds["zoneA"]);
assert(chunkBoundsUtil.containsKey(targetChunkBounds[0], ...originalZoneARange));
assert(chunkBoundsUtil.eq(targetChunkBounds[1], originalZoneARange[1]));
let remainingZoneAChunkBounds = zoneChunkBounds["zoneA"].filter(
    (chunkBounds) => !chunkBoundsUtil.eq(targetChunkBounds, chunkBounds));

jsTest.log(
    "Change the zone ranges so that the chunk that used to belong to zoneA now belongs to zoneB.");
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: originalZoneARange[0], max: originalZoneARange[1], zone: null}));
assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: ns,
    min: originalZoneARange[0],
    max: targetChunkBounds[0],
    zone: "zoneA"
}));
assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: ns,
    min: targetChunkBounds[0],
    max: originalZoneARange[1],
    zone: "zoneB"
}));

jsTest.log("Check that the chunk moves from zoneA to zoneB after the zone range change.");
runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: zoneChunkBounds["zoneC"],
    [st.shard1.shardName]: [targetChunkBounds, ...zoneChunkBounds["zoneB"]],
    [st.shard2.shardName]: remainingZoneAChunkBounds
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log(
    "Change the zone ranges so that the chunk that used to belong to zoneB now belongs to zoneC.");
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: targetChunkBounds[0], max: targetChunkBounds[1], zone: null}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: targetChunkBounds[0], max: targetChunkBounds[1], zone: "zoneC"}));

jsTest.log("Check that the chunk moves from zoneB to zoneC after the zone range change.");
runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: [targetChunkBounds, ...zoneChunkBounds["zoneC"]],
    [st.shard1.shardName]: zoneChunkBounds["zoneB"],
    [st.shard2.shardName]: remainingZoneAChunkBounds
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

jsTest.log("Make the chunk not aligned with zone ranges.");
let splitPoint = {x: NumberLong(targetChunkBounds[1].x - 5000)};
assert(chunkBoundsUtil.containsKey(splitPoint, ...targetChunkBounds));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: targetChunkBounds[0], max: targetChunkBounds[1], zone: null}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: targetChunkBounds[0], max: splitPoint, zone: "zoneC"}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: splitPoint, max: targetChunkBounds[1], zone: "zoneA"}));

jsTest.log(
    "Check that the balancer splits the chunk and that all chunks and docs are on the right shards.");
runBalancer(st, 1);
shardChunkBounds = {
    [st.shard0.shardName]: [[targetChunkBounds[0], splitPoint], ...zoneChunkBounds["zoneC"]],
    [st.shard1.shardName]: zoneChunkBounds["zoneB"],
    [st.shard2.shardName]: [[splitPoint, targetChunkBounds[1]], ...remainingZoneAChunkBounds]
};
assertChunksOnShards(configDB, ns, shardChunkBounds);
assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey);

st.stop();
})();
