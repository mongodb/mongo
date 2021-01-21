/**
 * Tests the pre-splitting behaviour of compound hashed shard key, for both the case where the
 * prefix field is hashed, and where the hashed field is not the prefix.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({name: jsTestName(), shards: 3});
const dbname = "test";
const mongos = st.s0;
const db = st.getDB(dbname);
db.adminCommand({enablesharding: dbname});
st.ensurePrimaryShard('test', st.shard1.shardName);

/**
 * Test that 'shardCollection' command works when there is existing data in collection and does not
 * do pre-splitting.
 */
[{a: "hashed", rangeField1: 1, rangeField2: 1}, {rangeField1: 1, a: "hashed", rangeField2: 1}]
    .forEach(function(shardKey) {
        db.collWithData.drop();
        db.collWithData.insert({a: 1});
        db.collWithData.createIndex(shardKey);

        // Verify that command fails when 'numInitialChunks' is specified.
        assert.commandFailedWithCode(db.adminCommand({
            shardcollection: db.collWithData.getFullName(),
            key: shardKey,
            numInitialChunks: 500
        }),
                                     ErrorCodes.InvalidOptions);

        assert.commandWorked(
            db.adminCommand({shardcollection: db.collWithData.getFullName(), key: shardKey}));
        assert.eq(findChunksUtil.countChunksForNs(st.config, db.collWithData.getFullName()),
                  1,
                  "sharding non-empty collection should not pre-split");
    });

/**
 * Helper function to validate that the chunks ranges have all the shard key fields and each shard
 * has expected number of chunks.
 */
function checkValidChunks(coll, shardKey, expectedChunks) {
    const chunks = findChunksUtil.findChunksByNs(st.config, coll.getFullName()).toArray();
    let shardCountsMap =
        {[st.shard0.shardName]: 0, [st.shard1.shardName]: 0, [st.shard2.shardName]: 0};
    for (let chunk of chunks) {
        shardCountsMap[chunk.shard]++;

        const assertHasAllShardKeyFields = function(obj) {
            assert.eq(Object.keys(shardKey).length, Object.keys(obj).length, tojson(obj));
            for (let key in obj) {
                assert(key in shardKey, tojson(obj));
            }
        };
        assertHasAllShardKeyFields(chunk.min);
        assertHasAllShardKeyFields(chunk.max);
    }
    let index = 0;
    for (let shardName in shardCountsMap) {
        assert.eq(expectedChunks[index++],
                  shardCountsMap[shardName],
                  "Expected chunks did not match for " + shardName + ". " + tojson(chunks));
    }
}

//
// Test cases for compound hashed shard keys with hashed prefix.
//
let shardKey = {hashedField: "hashed", rangeField1: 1, rangeField2: 1};

// Supported: Hashed sharding + numInitialChunks + empty collection.
// Expected: Even chunk distribution.
assert.commandWorked(db.hashedCollEmpty.createIndex(shardKey));
let coll = db.hashedCollEmpty;
assert.commandWorked(
    mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey, numInitialChunks: 6}));
checkValidChunks(coll, shardKey, [2, 2, 2]);

// Supported: Hashed sharding + numInitialChunks + non-existent collection.
// Expected: Even chunk distribution and the remainder chunks on the first shard.
coll = db.hashedCollNonExistent;
assert.commandWorked(
    mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey, numInitialChunks: 8}));
checkValidChunks(coll, shardKey, [4, 2, 2]);

// When 'numInitialChunks' is one, primary shard should have the chunk.
coll = db.hashedNumInitialChunksOne;
assert.commandWorked(
    mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey, numInitialChunks: 1}));
checkValidChunks(coll, shardKey, [0, 1, 0]);

// Default pre-splitting assigns two chunks per shard.
coll = db.hashedDefaultPreSplit;
assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
checkValidChunks(coll, shardKey, [2, 2, 2]);

db.hashedPrefixColl.drop();

// 'presplitHashedZones' cannot be passed without setting up zones.
assert.commandFailedWithCode(db.adminCommand({
    shardcollection: db.hashedPrefixColl.getFullName(),
    key: shardKey,
    numInitialChunks: 500,
    presplitHashedZones: true
}),
                             31387);

// Verify that 'shardCollection' command will fail if the zones are set up incorrectly.
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: 'hashedPrefix'}));
assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: db.hashedPrefixColl.getFullName(),
    min: {hashedField: MinKey, rangeField1: MinKey, rangeField2: MinKey},
    max: {
        hashedField: MaxKey,
        rangeField1: MaxKey,
        rangeField2: MinKey
    },  // All fields should be MaxKey for a valid zone.
    zone: 'hashedPrefix'
}));
assert.commandFailedWithCode(db.adminCommand({
    shardcollection: db.hashedPrefixColl.getFullName(),
    key: shardKey,
    presplitHashedZones: true
}),
                             31412);

// 'numInitialChunks' is ignored when zones are present and 'presplitHashedZones' is not set.
// Creates chunks based on the zones.
assert.commandWorked(db.adminCommand(
    {shardcollection: db.hashedPrefixColl.getFullName(), key: shardKey, numInitialChunks: 2}));
checkValidChunks(
    db.hashedPrefixColl, shardKey, [1 /* Boundary chunk */, 0, 1 /* zone present in shard2 */]);

// Verify that 'shardCollection' command will pre-split chunks if a single zone is set up ranging
// from MinKey to MaxKey and 'presplitHashedZones' flag is set.
db.hashedPrefixColl.drop();
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'hashedPrefix'}));
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: 'hashedPrefix'}));
assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: db.hashedPrefixColl.getFullName(),
    min: {hashedField: MinKey, rangeField1: MinKey, rangeField2: MinKey},
    max: {hashedField: MaxKey, rangeField1: MaxKey, rangeField2: MaxKey},
    zone: 'hashedPrefix'
}));
assert.commandWorked(db.adminCommand({
    shardcollection: db.hashedPrefixColl.getFullName(),
    key: shardKey,
    presplitHashedZones: true
}));

// By default, we create two chunks per shard for each shard that contains at least one zone.
checkValidChunks(db.hashedPrefixColl, shardKey, [0, 2, 2]);

// Verify that 'shardCollection' command will pre-split chunks equally among all the eligible
// shards.
db.hashedPrefixColl.drop();
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'hashedPrefix'}));
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'hashedPrefix'}));
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: 'hashedPrefix'}));

assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: db.hashedPrefixColl.getFullName(),
    min: {hashedField: MinKey, rangeField1: MinKey, rangeField2: MinKey},
    max: {hashedField: MaxKey, rangeField1: MaxKey, rangeField2: MaxKey},
    zone: 'hashedPrefix'
}));
assert.commandWorked(db.adminCommand({
    shardcollection: db.hashedPrefixColl.getFullName(),
    key: shardKey,
    presplitHashedZones: true,
    numInitialChunks: 100
}));
checkValidChunks(db.hashedPrefixColl, shardKey, [34, 34, 34]);

//
// Test cases for compound hashed shard keys with non-hashed prefix.
//

/**
 * Helper function to create two non-overlapping interweaving tag ranges for each of the specified
 * zones.
 */
function createZoneRanges(coll, zoneNames) {
    let offsetFirstTag = 0, offsetSecondTag = 0;

    // Create zone ranges such that first zone has tag ranges [a, b) and [A, B). Second zone has tag
    // ranges [c, d) and [C, D). Third zone has tag ranges [e, f) and [E, f) so on.
    for (let zoneName of zoneNames) {
        assert.commandWorked(st.s.adminCommand({
            updateZoneKeyRange: coll.getFullName(),
            min: {
                rangeField1: String.fromCharCode(97 + offsetFirstTag++),
                hashedField: MinKey,
                rangeField2: MinKey
            },
            max: {
                rangeField1: String.fromCharCode(97 + offsetFirstTag++),
                hashedField: MinKey,
                rangeField2: MinKey
            },
            zone: zoneName
        }));

        assert.commandWorked(st.s.adminCommand({
            updateZoneKeyRange: coll.getFullName(),
            min: {
                rangeField1: String.fromCharCode(65 + offsetSecondTag++),
                hashedField: MinKey,
                rangeField2: MinKey
            },
            max: {
                rangeField1: String.fromCharCode(65 + offsetSecondTag++),
                hashedField: MinKey,
                rangeField2: MinKey
            },
            zone: zoneName
        }));
    }
}

/**
 * Helper function to set up two zones named 'nonHashedPrefix1' and 'nonHashedPrefix2' on 'shard0'.
 */
function setUpTwoZonesOnShard0(coll) {
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'nonHashedPrefix1'}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'nonHashedPrefix2'}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: 'nonHashedPrefix1'}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: 'nonHashedPrefix2'}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard2.shardName, zone: 'nonHashedPrefix2'}));

    createZoneRanges(coll, ['nonHashedPrefix1', 'nonHashedPrefix2']);
}

/**
 * Helper function to set up two zones such that 'nonHashedPrefix1' zone is assigned to shard0 and
 * shard1. 'nonHashedPrefix2' is assinged to shard1 and shard2.
 */
function setUpTwoZones(coll) {
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: 'nonHashedPrefix2'}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'nonHashedPrefix1'}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'nonHashedPrefix1'}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'nonHashedPrefix2'}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: 'nonHashedPrefix2'}));
    createZoneRanges(coll, ['nonHashedPrefix1', 'nonHashedPrefix2']);
}

shardKey = {
    rangeField1: 1,
    hashedField: "hashed",
    rangeField2: 1
};
db.coll.drop();
setUpTwoZonesOnShard0(db.coll);

// 'numInitialChunks' cannot be passed without 'presplitHashedZones'.
assert.commandFailedWithCode(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, numInitialChunks: 500}),
    ErrorCodes.InvalidOptions);

db.coll.drop();
// 'presplitHashedZones' cannot be passed without setting up zones.
assert.commandFailedWithCode(db.adminCommand({
    shardcollection: db.coll.getFullName(),
    key: shardKey,
    numInitialChunks: 500,
    presplitHashedZones: true
}),
                             31387);

// Verify that shardCollection command will fail if the zones are set up incorrectly.
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'nonHashedPrefix1'}));
assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: db.coll.getFullName(),
    min: {rangeField1: "A", hashedField: MinKey, rangeField2: MinKey},
    max: {rangeField1: "A", hashedField: MaxKey, rangeField2: MinKey},
    zone: 'nonHashedPrefix1'
}));
assert.commandFailedWithCode(db.adminCommand({
    shardcollection: db.coll.getFullName(),
    key: shardKey,
    numInitialChunks: 500,
    presplitHashedZones: true
}),
                             31390);

// Verify that 'presplitHashedZones' with 'numInitialChunks' works correctly when zones are set up.
db.coll.drop();
setUpTwoZones(db.coll);
assert.commandWorked(db.adminCommand({
    shardcollection: db.coll.getFullName(),
    key: shardKey,
    numInitialChunks: 500,
    presplitHashedZones: true
}));

// We need to create ceil(500/3) = 167 chunks per shard. Shard1 has 4 tags(2 per zone) while others
// shards have 2 tags. So we create ceil(167/4) = 42 per tag on shard1 = 168, while we create
// ceil(167/2) = 84 per tag on others. In addition, we create 5 chunks for boundaries which will be
// distributed among the three shards using round robin.
checkValidChunks(db.coll, shardKey, [170, 170, 169]);

// When 'numInitialChunks = 1'.
db.coll.drop();
setUpTwoZones(db.coll);
assert.commandWorked(db.adminCommand({
    shardcollection: db.coll.getFullName(),
    key: shardKey,
    numInitialChunks: 1,
    presplitHashedZones: true
}));

// The chunk distribution from zones should be [2, 2+2 (two zones), 2]. The 5 gap chunks should be
// distributed among three shards.
checkValidChunks(db.coll, shardKey, [4, 6, 3]);

// Verify that 'presplitHashedZones' uses default value of two per shard when 'numInitialChunks' is
// not passed.
db.coll.drop();
setUpTwoZonesOnShard0(db.coll);
assert.commandWorked(db.adminCommand(
    {shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}));

// Since only Shard0 has chunks, we create on chunk per tag on shard0. The 5 gap chunks should be
// distributed among three shards.
checkValidChunks(db.coll, shardKey, [6, 2, 1]);

st.stop();
})();