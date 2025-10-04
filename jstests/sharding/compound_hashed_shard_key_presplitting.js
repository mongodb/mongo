/**
 * Tests the pre-splitting behaviour of compound hashed shard key, for both the case where the
 * prefix field is hashed, and where the hashed field is not the prefix.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({name: jsTestName(), shards: 3});
const dbname = "test";
const mongos = st.s0;
const db = st.getDB(dbname);
db.adminCommand({enablesharding: dbname, primaryShard: st.shard1.shardName});

const expectedTotalChunkCount = 3;
const expectedChunkPerShardCount = 1;

/**
 * Test that 'shardCollection' command works when there is existing data in collection and does not
 * do pre-splitting.
 */
[
    {a: "hashed", rangeField1: 1, rangeField2: 1},
    {rangeField1: 1, a: "hashed", rangeField2: 1},
].forEach(function (shardKey) {
    db.collWithData.drop();
    db.collWithData.insert({a: 1});
    db.collWithData.createIndex(shardKey);

    assert.commandWorked(db.adminCommand({shardcollection: db.collWithData.getFullName(), key: shardKey}));
    assert.eq(
        findChunksUtil.countChunksForNs(st.config, db.collWithData.getFullName()),
        1,
        "sharding non-empty collection should not pre-split",
    );
});

/**
 * Helper function to validate that the chunks ranges have all the shard key fields and each shard
 * has expected number of chunks.
 */
function checkValidChunks(coll, shardKey, checkChunksPerShardFn) {
    const chunks = findChunksUtil.findChunksByNs(st.config, coll.getFullName()).toArray();
    let shardCountsMap = {[st.shard0.shardName]: 0, [st.shard1.shardName]: 0, [st.shard2.shardName]: 0};
    for (let chunk of chunks) {
        shardCountsMap[chunk.shard]++;

        const assertHasAllShardKeyFields = function (obj) {
            assert.eq(Object.keys(shardKey).length, Object.keys(obj).length, tojson(obj));
            for (let key in obj) {
                assert(key in shardKey, tojson(obj));
            }
        };
        assertHasAllShardKeyFields(chunk.min);
        assertHasAllShardKeyFields(chunk.max);
    }

    checkChunksPerShardFn(shardCountsMap);
}

//
// Test cases for compound hashed shard keys with hashed prefix.
//
let shardKey = {hashedField: "hashed", rangeField1: 1, rangeField2: 1};

// Supported: Hashed sharding + empty collection.
// Expected: Chunk distribution with a total of 'expectedTotalChunkCount' chunks,
// and 'expectedChunkPerShardCount' chunks placed on each shard.
assert.commandWorked(db.hashedCollEmpty.createIndex(shardKey));
let coll = db.hashedCollEmpty;
assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
checkValidChunks(coll, shardKey, (shardCountsMap) => {
    // Each shard has 'expectedChunkPerShardCount' chunks.
    assert(Object.values(shardCountsMap).every((count) => count === expectedChunkPerShardCount));
});

// Supported: Hashed sharding + non-existent collection.
// Expected: Chunk distribution with a total of 'expectedTotalChunkCount' chunks,
// and 'expectedChunkPerShardCount' chunks placed on each shard.
coll = db.hashedCollNonExistent;
assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
checkValidChunks(coll, shardKey, (shardCountsMap) => {
    const totalChunks = Object.values(shardCountsMap).reduce((accumulator, v) => accumulator + v);
    assert.eq(expectedTotalChunkCount, totalChunks, "Unexpected total amount of chunks");
    // Each shard has 'expectedChunkPerShardCount' chunks.
    assert(Object.values(shardCountsMap).every((count) => count == expectedChunkPerShardCount));
});

// Default pre-splitting assigns one chunk per shard.
coll = db.hashedDefaultPreSplit;
assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
checkValidChunks(coll, shardKey, (shardCountsMap) => {
    assert.gte(
        shardCountsMap[st.shard0.shardName],
        expectedChunkPerShardCount,
        "Unexpected amount of chunks on " + st.shard0.shardName,
    );
    assert.gte(
        shardCountsMap[st.shard1.shardName],
        expectedChunkPerShardCount,
        "Unexpected amount of chunks on " + st.shard1.shardName,
    );
    assert.gte(
        shardCountsMap[st.shard2.shardName],
        expectedChunkPerShardCount,
        "Unexpected amount of chunks on " + st.shard2.shardName,
    );
});

db.hashedPrefixColl.drop();

// 'presplitHashedZones' cannot be passed without setting up zones.
assert.commandFailedWithCode(
    db.adminCommand({
        shardcollection: db.hashedPrefixColl.getFullName(),
        key: shardKey,
        presplitHashedZones: true,
    }),
    31387,
);

// Verify that 'shardCollection' command will fail if the zones are set up incorrectly.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "hashedPrefix"}));
assert.commandWorked(
    st.s.adminCommand({
        updateZoneKeyRange: db.hashedPrefixColl.getFullName(),
        min: {hashedField: MinKey, rangeField1: MinKey, rangeField2: MinKey},
        max: {
            hashedField: MaxKey,
            rangeField1: MaxKey,
            rangeField2: MinKey,
        }, // All fields should be MaxKey for a valid zone.
        zone: "hashedPrefix",
    }),
);
assert.commandFailedWithCode(
    db.adminCommand({
        shardcollection: db.hashedPrefixColl.getFullName(),
        key: shardKey,
        presplitHashedZones: true,
    }),
    31412,
);

// Verify that 'shardCollection' command will pre-split chunks if a single zone is set up ranging
// from MinKey to MaxKey and 'presplitHashedZones' flag is set.
db.hashedPrefixColl.drop();
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "hashedPrefix"}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "hashedPrefix"}));
assert.commandWorked(
    st.s.adminCommand({
        updateZoneKeyRange: db.hashedPrefixColl.getFullName(),
        min: {hashedField: MinKey, rangeField1: MinKey, rangeField2: MinKey},
        max: {hashedField: MaxKey, rangeField1: MaxKey, rangeField2: MaxKey},
        zone: "hashedPrefix",
    }),
);
assert.commandWorked(
    db.adminCommand({
        shardcollection: db.hashedPrefixColl.getFullName(),
        key: shardKey,
        presplitHashedZones: true,
    }),
);

// By default, we create one chunk per shard for each shard that contains at least one zone.
checkValidChunks(db.hashedPrefixColl, shardKey, (shardCountsMap) => {
    assert.eq(0, shardCountsMap[st.shard0.shardName], "Unexpected amount of chunks on shard0");
    assert.eq(expectedChunkPerShardCount, shardCountsMap[st.shard1.shardName], "Unexpected amount of chunks on shard1");
    assert.eq(expectedChunkPerShardCount, shardCountsMap[st.shard2.shardName], "Unexpected amount of chunks on shard2");
});

//
// Test cases for compound hashed shard keys with non-hashed prefix.
//

/**
 * Helper function to create two non-overlapping interweaving tag ranges for each of the specified
 * zones.
 */
function createZoneRanges(coll, zoneNames) {
    let offsetFirstTag = 0,
        offsetSecondTag = 0;

    // Create zone ranges such that first zone has tag ranges [a, b) and [A, B). Second zone has tag
    // ranges [c, d) and [C, D). Third zone has tag ranges [e, f) and [E, f) so on.
    for (let zoneName of zoneNames) {
        assert.commandWorked(
            st.s.adminCommand({
                updateZoneKeyRange: coll.getFullName(),
                min: {
                    rangeField1: String.fromCharCode(97 + offsetFirstTag++),
                    hashedField: MinKey,
                    rangeField2: MinKey,
                },
                max: {
                    rangeField1: String.fromCharCode(97 + offsetFirstTag++),
                    hashedField: MinKey,
                    rangeField2: MinKey,
                },
                zone: zoneName,
            }),
        );

        assert.commandWorked(
            st.s.adminCommand({
                updateZoneKeyRange: coll.getFullName(),
                min: {
                    rangeField1: String.fromCharCode(65 + offsetSecondTag++),
                    hashedField: MinKey,
                    rangeField2: MinKey,
                },
                max: {
                    rangeField1: String.fromCharCode(65 + offsetSecondTag++),
                    hashedField: MinKey,
                    rangeField2: MinKey,
                },
                zone: zoneName,
            }),
        );
    }
}

/**
 * Helper function to set up two zones named 'nonHashedPrefix1' and 'nonHashedPrefix2' on 'shard0'.
 */
function setUpTwoZonesOnShard0(coll) {
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "nonHashedPrefix1"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "nonHashedPrefix2"}));
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: "nonHashedPrefix1"}));
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: "nonHashedPrefix2"}));
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard2.shardName, zone: "nonHashedPrefix2"}));

    createZoneRanges(coll, ["nonHashedPrefix1", "nonHashedPrefix2"]);
}

/**
 * Helper function to set up two zones such that 'nonHashedPrefix1' zone is assigned to shard0 and
 * shard1. 'nonHashedPrefix2' is assinged to shard1 and shard2.
 */
function setUpTwoZones(coll) {
    assert.commandWorked(st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "nonHashedPrefix2"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "nonHashedPrefix1"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "nonHashedPrefix1"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "nonHashedPrefix2"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: "nonHashedPrefix2"}));
    createZoneRanges(coll, ["nonHashedPrefix1", "nonHashedPrefix2"]);
}

shardKey = {
    rangeField1: 1,
    hashedField: "hashed",
    rangeField2: 1,
};
db.coll.drop();
setUpTwoZonesOnShard0(db.coll);

db.coll.drop();
// 'presplitHashedZones' cannot be passed without setting up zones.
assert.commandFailedWithCode(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}),
    31387,
);

// Verify that shardCollection command will fail if the zones are set up incorrectly.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "nonHashedPrefix1"}));
assert.commandWorked(
    st.s.adminCommand({
        updateZoneKeyRange: db.coll.getFullName(),
        min: {rangeField1: "A", hashedField: MinKey, rangeField2: MinKey},
        max: {rangeField1: "A", hashedField: MaxKey, rangeField2: MinKey},
        zone: "nonHashedPrefix1",
    }),
);
assert.commandFailedWithCode(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}),
    31390,
);

// Verify that 'presplitHashedZones' works correctly when zones are set up.
db.coll.drop();
setUpTwoZones(db.coll);
assert.commandWorked(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}),
);

// Verify that 'presplitHashedZones' uses the default initial value of 1 chunk per tag on each
// shard. Setup two zones such that shard1 hosts both zones, and shard0/2 host one zone each.
db.coll.drop();
setUpTwoZones(db.coll);
assert.commandWorked(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}),
);

// The chunk distribution from zones should be [2, 2+2 (two zones), 2]. The 5 gap chunks should be
// distributed among three shards.
checkValidChunks(db.coll, shardKey, (shardCountsMap) => {
    const totalChunks = Object.values(shardCountsMap).reduce((accumulator, v) => accumulator + v);
    assert.eq(13, totalChunks, "Unexpected total amount of chunks");

    assert.gte(shardCountsMap[st.shard0.shardName], 3, "Unexpected amount of chunks on shard0");
    assert.gte(shardCountsMap[st.shard1.shardName], 5, "Unexpected amount of chunks on shard1");
    assert.gte(shardCountsMap[st.shard2.shardName], 3, "Unexpected amount of chunks on shard2");
});

// Verify that 'presplitHashedZones' uses the default initial value of 1 chunk per tag on each
// shard. Setup two zones on shard0.
db.coll.drop();
setUpTwoZonesOnShard0(db.coll);
assert.commandWorked(
    db.adminCommand({shardcollection: db.coll.getFullName(), key: shardKey, presplitHashedZones: true}),
);

// Since only Shard0 has chunks, we create on chunk per tag on shard0. The 5 gap chunks should be
// distributed among three shards.
checkValidChunks(db.coll, shardKey, (shardCountsMap) => {
    const totalChunks = Object.values(shardCountsMap).reduce((accumulator, v) => accumulator + v);
    assert.eq(9, totalChunks, "Unexpected total amount of chunks");

    assert.gte(shardCountsMap[st.shard0.shardName], 5, "Unexpected amount of chunks on shard0");
    assert.gte(shardCountsMap[st.shard1.shardName], 1, "Unexpected amount of chunks on shard1");
    assert.gte(shardCountsMap[st.shard2.shardName], 1, "Unexpected amount of chunks on shard2");
});

st.stop();
