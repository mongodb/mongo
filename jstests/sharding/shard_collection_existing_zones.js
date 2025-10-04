// Test that shardCollection uses existing zone info to validate
// shard keys and do initial chunk splits.
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 3});
let kDbName = "test";
let kCollName = "foo";
let ns = kDbName + "." + kCollName;
let zoneName = "zoneName";
let mongos = st.s0;
let testDB = mongos.getDB(kDbName);
let configDB = mongos.getDB("config");
let shardName = st.shard0.shardName;
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

/**
 * Test that shardCollection correctly validates that a zone is associated with a shard.
 */
function testShardZoneAssociationValidation(proposedShardKey, numberLongMin, numberLongMax) {
    let zoneMin = numberLongMin ? {x: NumberLong(0)} : {x: 0};
    let zoneMax = numberLongMax ? {x: NumberLong(10)} : {x: 10};
    assert.commandWorked(
        configDB.tags.insert({_id: {ns: ns, min: zoneMin}, ns: ns, min: zoneMin, max: zoneMax, tag: zoneName}),
    );

    let tagDoc = configDB.tags.findOne();
    assert.eq(ns, tagDoc.ns);
    assert.eq(zoneMin, tagDoc.min);
    assert.eq(zoneMax, tagDoc.max);
    assert.eq(zoneName, tagDoc.tag);

    assert.commandFailed(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));

    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zoneName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));

    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

/**
 * Test that shardCollection correctly validates shard key against existing zones.
 */
function testShardKeyValidation(proposedShardKey, numberLongMin, numberLongMax, success) {
    assert.commandWorked(testDB.foo.createIndex(proposedShardKey));
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zoneName}));

    let zoneMin = numberLongMin ? {x: NumberLong(0)} : {x: 0};
    let zoneMax = numberLongMax ? {x: NumberLong(10)} : {x: 10};
    assert.commandWorked(st.s.adminCommand({updateZoneKeyRange: ns, min: zoneMin, max: zoneMax, zone: zoneName}));

    let tagDoc = configDB.tags.findOne();
    jsTestLog("xxx tag doc " + tojson(tagDoc));
    assert.eq(ns, tagDoc.ns);
    assert.eq(zoneMin, tagDoc.min);
    assert.eq(zoneMax, tagDoc.max);
    assert.eq(zoneName, tagDoc.tag);

    if (success) {
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));
    } else {
        assert.commandFailed(mongos.adminCommand({shardCollection: ns, key: proposedShardKey}));
    }

    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

/**
 * Test that shardCollection uses existing zone ranges to split chunks.
 */
function testChunkSplits(collectionExists) {
    let shardKey = {x: 1};
    let ranges = [
        {min: {x: 0}, max: {x: 10}},
        {min: {x: 10}, max: {x: 20}},
        {min: {x: 30}, max: {x: 40}},
    ];
    let shards = configDB.shards.find().toArray();
    assert.eq(ranges.length, shards.length);
    if (collectionExists) {
        assert.commandWorked(testDB.foo.createIndex(shardKey));
    }

    // create zones:
    // shard0 - zonename0 - [0, 10)
    // shard1 - zonename1 - [10, 20)
    // shard2 - zonename2 - [30, 40)
    for (let i = 0; i < shards.length; i++) {
        assert.commandWorked(st.s.adminCommand({addShardToZone: shards[i]._id, zone: zoneName + i}));
        assert.commandWorked(
            st.s.adminCommand({updateZoneKeyRange: ns, min: ranges[i].min, max: ranges[i].max, zone: zoneName + i}),
        );
    }
    assert.eq(configDB.tags.find().count(), shards.length, "failed to create tag documents correctly");
    assert.eq(
        configDB.chunks.find({ns: ns}).count(),
        0,
        "expect to see no chunk documents for the collection before shardCollection is run",
    );

    // shard the collection and validate the resulting chunks
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));
    // Expected:
    //   - zoned chunks on the corresponding shard.
    //   - 2 chunks on each shard
    let expectedChunks = [
        {range: [{x: {"$minKey": 1}}, {x: 0}], shardId: null}, // any shard
        {range: [{x: 0}, {x: 10}], shardId: st.shard0.shardName}, // pre-defined
        {range: [{x: 10}, {x: 20}], shardId: st.shard1.shardName}, // pre-defined
        {range: [{x: 20}, {x: 30}], shardId: null}, // any shard
        {range: [{x: 30}, {x: 40}], shardId: st.shard2.shardName}, // pre-defined
        {range: [{x: 40}, {x: {"$maxKey": 1}}], shardId: null}, // any shard
    ];
    let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).sort({min: 1}).toArray();
    assert.eq(chunkDocs.length, expectedChunks.length, "shardCollection failed to create chunk documents correctly");
    for (let i = 0; i < chunkDocs.length; i++) {
        let errMsg = "expect to see chunk " + tojson(expectedChunks[i]) + " but found chunk " + tojson(chunkDocs[i]);
        assert.eq(expectedChunks[i].range[0], chunkDocs[i].min, errMsg);
        assert.eq(expectedChunks[i].range[1], chunkDocs[i].max, errMsg);
        if (expectedChunks[i].shardId !== null) {
            assert.eq(expectedChunks[i].shardId, chunkDocs[i].shard, errMsg);
        }
    }
    assert.eq(2, findChunksUtil.countChunksForNs(configDB, ns, {shard: st.shard0.shardName}));
    assert.eq(2, findChunksUtil.countChunksForNs(configDB, ns, {shard: st.shard1.shardName}));
    assert.eq(2, findChunksUtil.countChunksForNs(configDB, ns, {shard: st.shard2.shardName}));

    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

/**
 * Tests that a non-empty collection associated with zones can be sharded.
 */
function testNonemptyZonedCollection() {
    let shardKey = {x: 1};
    let shards = configDB.shards.find().toArray();
    let testColl = testDB.getCollection(kCollName);
    let ranges = [
        {min: {x: 0}, max: {x: 10}},
        {min: {x: 10}, max: {x: 20}},
        {min: {x: 20}, max: {x: 40}},
    ];

    for (let i = 0; i < 40; i++) {
        assert.commandWorked(testColl.insert({x: i}));
    }

    assert.commandWorked(testColl.createIndex(shardKey));

    for (let i = 0; i < shards.length; i++) {
        assert.commandWorked(mongos.adminCommand({addShardToZone: shards[i]._id, zone: zoneName + i}));
        assert.commandWorked(
            mongos.adminCommand({updateZoneKeyRange: ns, min: ranges[i].min, max: ranges[i].max, zone: zoneName + i}),
        );
    }

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));

    // Check that there is initially 1 chunk.
    assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));

    st.startBalancer();

    // Check that the chunks were moved properly.
    assert.soon(
        () => {
            let res = findChunksUtil.countChunksForNs(configDB, ns);
            return res === 5;
        },
        "balancer never ran",
        10 * 60 * 1000,
        1000,
    );

    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

// test that shardCollection checks that a zone is associated with a shard.
testShardZoneAssociationValidation({x: 1}, false, false);

// test that shardCollection uses existing zones to validate shard key
testShardKeyValidation({x: 1}, false, false, true);

// cannot use a completely different key from the zone shard key or a key
// that has the zone shard key as a prefix is not allowed.
testShardKeyValidation({y: 1}, false, false, false);
testShardKeyValidation({x: 1, y: 1}, false, false, false);

// can only do hash sharding when the boundaries are of type NumberLong.
testShardKeyValidation({x: "hashed"}, false, false, false);
testShardKeyValidation({x: "hashed"}, true, false, false);
testShardKeyValidation({x: "hashed"}, false, true, false);
testShardKeyValidation({x: "hashed"}, true, true, true);

assert.commandWorked(st.s.adminCommand({removeShardFromZone: shardName, zone: zoneName}));

// test that shardCollection uses zone ranges to split chunks

testChunkSplits(false);
testChunkSplits(true);

testNonemptyZonedCollection();

st.stop();
