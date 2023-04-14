/**
 * Testing that config.tags are correctly updated after resharding hashed shard key with zones.
 */

(function() {
"use strict";

const st = new ShardingTest({shard: 2});
const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: "hashed"}}));

const existingZoneName = 'x1';
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));

assert.commandWorked(st.s.adminCommand({
    updateZoneKeyRange: ns,
    min: {oldKey: NumberLong("4470791281878691347")},
    max: {oldKey: NumberLong("7766103514953448109")},
    zone: existingZoneName
}));

assert.commandWorked(st.s.adminCommand({
    reshardCollection: ns,
    key: {oldKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{
        zone: existingZoneName,
        min: {oldKey: NumberLong("4470791281878691346")},
        max: {oldKey: NumberLong("7766103514953448108")}
    }],
    numInitialChunks: 2,
}));

// Find the tags docs.
var configDB = st.s.getDB("config");
let tags = configDB.tags.find({}).toArray();

// Assert only one tag doc is present and zone ranges are correct.
assert.eq(1, configDB.tags.countDocuments({}));
assert.eq({oldKey: NumberLong("4470791281878691346")}, tags[0].min);
assert.eq({oldKey: NumberLong("7766103514953448108")}, tags[0].max);
assert.eq(existingZoneName, tags[0].tag);

st.stop();
})();
