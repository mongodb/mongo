/**
 * Test that updateZoneKeyRange command works correctly in combination with shardCollection command.
 * In this test we verify the behaviour of:
 *  - Creating zones after sharding the collection.
 *  - Creating zones before sharding the collection.
 *  - Creating zones in collection which has data and then sharding the collection.
 *
 * @tags: [requires_fcv_44, multiversion_incompatible]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 3});
const kDbName = 'test';
const kCollName = 'foo';
const ns = kDbName + '.' + kCollName;
const zoneName = 'zoneName';
const mongos = st.s0;
const testDB = mongos.getDB(kDbName);
const configDB = mongos.getDB('config');
const shardName = st.shard0.shardName;
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: 'zoneName'}));

function fillMissingShardKeyFields(shardKey, doc, value) {
    for (let key in shardKey) {
        if (!(key in doc)) {
            doc[key] = value ? value : {"$minKey": 1};
        }
    }
    return doc;
}
/**
 * Test that 'updateZoneKeyRange' works correctly by verifying 'tags' collection, after sharding the
 * collection.
 */
function testZoningAfterSharding(namespace, shardKey, NumberType) {
    assert.commandWorked(st.s.adminCommand({shardCollection: namespace, key: shardKey}));

    if (shardKey.x === "hashed") {
        // Cannot assign with a non-NumberLong range value on a hashed shard key field.
        assert.commandFailedWithCode(
            st.s.adminCommand(
                {updateZoneKeyRange: namespace, min: {x: 0}, max: {x: 10}, zone: 'zoneName'}),
            ErrorCodes.InvalidOptions);
    }

    // Testing basic assign.
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: namespace,
        min: {x: NumberType(0)},
        max: {x: NumberType(10)},
        zone: 'zoneName'
    }));

    let tagDoc = configDB.tags.findOne();

    assert.eq(namespace, tagDoc.ns);
    assert.eq(fillMissingShardKeyFields(shardKey, {x: NumberType(0)}), tagDoc.min);
    assert.eq(fillMissingShardKeyFields(shardKey, {x: NumberType(10)}), tagDoc.max);
    assert.eq('zoneName', tagDoc.tag);

    // Cannot assign overlapping ranges
    assert.commandFailedWithCode(st.s.adminCommand({
        updateZoneKeyRange: namespace,
        min: {x: NumberType(-10)},
        max: {x: NumberType(20)},
        zone: 'zoneName'
    }),
                                 ErrorCodes.RangeOverlapConflict);

    // Cannot have non-shard key fields in tag range.
    assert.commandFailedWithCode(st.s.adminCommand({
        updateZoneKeyRange: namespace,
        min: {newField: NumberType(-10)},
        max: {newField: NumberType(20)},
        zone: 'zoneName'
    }),
                                 ErrorCodes.ShardKeyNotFound);

    tagDoc = configDB.tags.findOne();
    assert.eq(namespace, tagDoc.ns);
    assert.eq(fillMissingShardKeyFields(shardKey, {x: NumberType(0)}), tagDoc.min);
    assert.eq(fillMissingShardKeyFields(shardKey, {x: NumberType(10)}), tagDoc.max);
    assert.eq('zoneName', tagDoc.tag);

    // Testing basic remove.
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: namespace,
        min: fillMissingShardKeyFields(shardKey, {x: NumberType(0)}, MinKey),
        max: fillMissingShardKeyFields(shardKey, {x: NumberType(10)}, MinKey),
        zone: null
    }));
    assert.eq(null, configDB.tags.findOne());

    // Insert directly into the tags collection.
    const zone = {
        _id: 0,
        ns: namespace,
        min: fillMissingShardKeyFields(shardKey, {x: 0}, MinKey),
        max: fillMissingShardKeyFields(shardKey, {x: 10}, MinKey),
        zone: "zoneName"
    };
    assert.commandWorked(configDB.tags.insert(zone));
    assert.eq(zone, configDB.tags.findOne());

    // Remove works on entries inserted directly into the tags collection, even when those entries
    // do not adhere to the updateZoneKeyRange command requirement of having a NumberLong range
    // value for a hashed shard key field.
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: namespace,
        min: fillMissingShardKeyFields(shardKey, {x: 0}, MinKey),
        max: fillMissingShardKeyFields(shardKey, {x: 10}, MinKey),
        zone: null
    }));
    assert.eq(null, configDB.tags.findOne());
}

testZoningAfterSharding("test.compound_hashed", {x: 1, y: "hashed", z: 1}, Number);
testZoningAfterSharding("test.compound_hashed", {x: 1, y: "hashed", z: 1}, NumberLong);
testZoningAfterSharding("test.compound_hashed_prefix", {x: "hashed", y: 1, z: 1}, NumberLong);

/**
 * Test that shardCollection correctly validates shard key against existing zones.
 */
function testZoningBeforeSharding({shardKey, zoneRange, failCode}) {
    assert.commandWorked(testDB.foo.createIndex(shardKey));
    assert.commandWorked(st.s.adminCommand({addShardToZone: shardName, zone: zoneName}));

    // Update zone range and verify that the 'tags' collection is updated appropriately.
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: zoneRange[0], max: zoneRange[1], zone: zoneName}));
    assert.eq(1, configDB.tags.count({ns: ns, min: zoneRange[0], max: zoneRange[1]}));

    if (failCode) {
        assert.commandFailedWithCode(mongos.adminCommand({shardCollection: ns, key: shardKey}),
                                     failCode);
    } else {
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));
    }
    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

// Fails when hashed field is not number long in 'zoneRange'.
testZoningBeforeSharding(
    {shardKey: {x: "hashed"}, zoneRange: [{x: -5}, {x: 5}], failCode: ErrorCodes.InvalidOptions});
testZoningBeforeSharding({
    shardKey: {x: "hashed"},
    zoneRange: [{x: NumberLong(-5)}, {x: 5}],
    failCode: ErrorCodes.InvalidOptions
});
testZoningBeforeSharding({
    shardKey: {x: "hashed"},
    zoneRange: [{x: -5}, {x: NumberLong(5)}],
    failCode: ErrorCodes.InvalidOptions
});
testZoningBeforeSharding(
    {shardKey: {x: "hashed"}, zoneRange: [{x: NumberLong(-5)}, {x: NumberLong(5)}]});
testZoningBeforeSharding({
    shardKey: {x: "hashed", y: 1},
    zoneRange: [{x: NumberLong(-5), y: MinKey}, {x: NumberLong(5), y: MinKey}]
});
testZoningBeforeSharding({
    shardKey: {x: 1, y: "hashed"},
    zoneRange: [{x: 1, y: NumberLong(-5)}, {x: 2, y: NumberLong(5)}]
});
testZoningBeforeSharding({
    shardKey: {x: 1, y: "hashed"},
    zoneRange: [{x: 1, y: NumberLong(-5)}, {x: 2, y: 5}],
    failCode: ErrorCodes.InvalidOptions
});

// Fails when 'zoneRange' doesn't have a shard key field.
testZoningBeforeSharding({
    shardKey: {x: 1, y: "hashed", z: 1},
    zoneRange: [{x: 1, y: NumberLong(-5)}, {x: 2, y: NumberLong(5)}],
    failCode: ErrorCodes.InvalidOptions
});

// Works when shard key field is defined as 'MinKey'.
testZoningBeforeSharding({
    shardKey: {x: 1, y: "hashed", z: 1},
    zoneRange: [{x: 1, y: NumberLong(-5), z: MinKey}, {x: 2, y: NumberLong(5), z: MinKey}],
});
testZoningBeforeSharding(
    {shardKey: {x: 1, y: "hashed"}, zoneRange: [{x: "DUB", y: MinKey}, {x: "NYC", y: MinKey}]});

assert.commandWorked(st.s.adminCommand({removeShardFromZone: shardName, zone: zoneName}));

/**
 * Test that shardCollection uses existing zone ranges to split chunks.
 */
function testChunkSplits({collectionExists, shardKey, zoneRanges, expectedNumChunks}) {
    const shards = configDB.shards.find().toArray();
    if (collectionExists) {
        assert.commandWorked(testDB.foo.createIndex(shardKey));
    }

    // Create a new zone and assign each zone to the shards using round-robin. Then update each of
    // the zone's range to the range specified in 'zoneRanges'.
    for (let i = 0; i < zoneRanges.length; i++) {
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: shards[i % shards.length]._id, zone: zoneName + i}));
        assert.commandWorked(st.s.adminCommand({
            updateZoneKeyRange: ns,
            min: zoneRanges[i][0],
            max: zoneRanges[i][1],
            zone: zoneName + i
        }));
    }
    assert.eq(configDB.tags.count({ns: ns}), zoneRanges.length);
    assert.eq(0,
              configDB.chunks.count({ns: ns}),
              "expect to see no chunk documents for the collection before shardCollection is run");

    // Shard the collection and validate the resulting chunks.
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));
    const chunkDocs = configDB.chunks.find({ns: ns}).toArray();
    assert.eq(chunkDocs.length, expectedNumChunks, chunkDocs);

    // Verify that each of the chunks corresponding to zones are in the right shard.
    for (let i = 0; i < zoneRanges.length; i++) {
        assert.eq(1,
                  configDB.chunks.count({
                      ns: ns,
                      min: zoneRanges[i][0],
                      max: zoneRanges[i][1],
                      shard: shards[i % shards.length]._id
                  }),
                  chunkDocs);
    }
    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

// When shard key is compound hashed with range prefix.
testChunkSplits({
    shardKey: {x: 1, y: "hashed"},
    zoneRanges: [
        [{x: 0, y: MinKey}, {x: 5, y: MinKey}],
        [{x: 10, y: MinKey}, {x: 15, y: MinKey}],
        [{x: 20, y: MinKey}, {x: 25, y: MinKey}],
        [{x: 30, y: MinKey}, {x: 35, y: MinKey}],
    ],
    expectedNumChunks: 9  // 4 zones + 2 boundaries + 3 gap chunks.
});
testChunkSplits({
    shardKey: {x: 1, y: "hashed", z: 1},
    zoneRanges: [
        [{x: 0, y: NumberLong(0), z: MinKey}, {x: 5, y: NumberLong(0), z: MinKey}],
        [{x: 10, y: NumberLong(0), z: MinKey}, {x: 15, y: NumberLong(0), z: MinKey}],
        [{x: 20, y: NumberLong(0), z: MinKey}, {x: 25, y: NumberLong(0), z: MinKey}],
        [{x: 30, y: NumberLong(0), z: MinKey}, {x: 35, y: NumberLong(0), z: MinKey}],
    ],
    expectedNumChunks: 9  // 4 zones + 2 boundaries + 3 gap chunks.
});

// When shard key is compound hashed with hashed prefix.
testChunkSplits({
    collectionExists: true,
    shardKey: {x: "hashed", y: 1},
    zoneRanges: [
        [{x: NumberLong(0), y: MinKey}, {x: NumberLong(10), y: MinKey}],
        [{x: NumberLong(10), y: MinKey}, {x: NumberLong(20), y: MinKey}],
        [{x: NumberLong(20), y: MinKey}, {x: NumberLong(30), y: MinKey}],
        [{x: NumberLong(30), y: MinKey}, {x: NumberLong(40), y: MinKey}],
        [{x: NumberLong(40), y: MinKey}, {x: NumberLong(50), y: MinKey}],
    ],
    expectedNumChunks: 7  // 5 zones + 2 boundaries.
});

/**
 * Tests that a non-empty collection associated with zones can be sharded.
 */
function testNonemptyZonedCollection() {
    const shardKey = {x: 1, y: "hashed"};
    const shards = configDB.shards.find().toArray();
    const testColl = testDB.getCollection(kCollName);
    const ranges = [
        {min: {x: 0, y: MinKey}, max: {x: 10, y: MaxKey}},
        {min: {x: 10, y: MaxKey}, max: {x: 20, y: MinKey}},
        {min: {x: 20, y: MinKey}, max: {x: 40, y: MaxKey}}
    ];

    for (let i = 0; i < 40; i++) {
        assert.commandWorked(testColl.insert({x: 1, y: Math.random()}));
    }

    assert.commandWorked(testColl.createIndex(shardKey));

    for (let i = 0; i < shards.length; i++) {
        assert.commandWorked(
            mongos.adminCommand({addShardToZone: shards[i]._id, zone: zoneName + i}));
        assert.commandWorked(mongos.adminCommand(
            {updateZoneKeyRange: ns, min: ranges[i].min, max: ranges[i].max, zone: zoneName + i}));
    }

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));

    // Check that there is initially 1 chunk.
    assert.eq(1, configDB.chunks.count({ns: ns}));

    st.startBalancer();

    // Check that the chunks were moved properly.
    assert.soon(
        () => configDB.chunks.count({ns: ns}) === 5, 'balancer never ran', 5 * 60 * 1000, 1000);

    assert.commandWorked(testDB.runCommand({drop: kCollName}));
}

testNonemptyZonedCollection();

st.stop();
})();
