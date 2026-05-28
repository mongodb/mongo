/**
 * Tests that config.tags zone ranges are correctly updated after resharding a timeseries
 * collection. Zone boundaries are specified using the user-facing metaField name ("sensorId");
 * after resharding, config.tags should store the translated internal "meta" field name —
 * exercising the zone range translation path (user-facing -> internal bucket field).
 *
 * Timeseries variant of resharding_update_tag_zones.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const st = new ShardingTest({
    shards: 2,
    configOptions: {
        setParameter: {"reshardingCriticalSectionTimeoutMillis": 24 * 60 * 60 * 1000 /* 1 day */},
    },
});
const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create a timeseries collection with a non-"meta" metaField name to exercise the shard key
// translation path (user-facing field -> internal bucket field).
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {"sensorId.x": 1},
        timeseries: {timeField: "ts", metaField: "sensorId"},
    }),
);

const existingZoneName = "x1";
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));

// Set up an initial zone range on the existing shard key. updateZoneKeyRange requires the
// internal field name ("meta.x") since config.collections stores the translated shard key.
// This zone should be replaced by the new one specified in the resharding command.
assert.commandWorked(
    st.s.adminCommand({
        updateZoneKeyRange: ns,
        min: {"meta.x": MinKey},
        max: {"meta.x": 0},
        zone: existingZoneName,
    }),
);

// Reshard to sensorId.y with a new zone range. The reshardCollection key uses the user-facing
// metaField name ("sensorId.y"), but zone boundaries must use the internal "meta" field name
// since they are validated against the translated key pattern.
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: ns,
        key: {"sensorId.y": 1},
        unique: false,
        collation: {locale: "simple"},
        zones: [
            {
                zone: existingZoneName,
                min: {"meta.y": MinKey},
                max: {"meta.y": 0},
            },
        ],
        numInitialChunks: 2,
    }),
);

// config.collections and config.tags store the bucket namespace for viewful timeseries (FCV < 9.0)
// and the user-facing namespace for viewless timeseries (FCV >= 9.0).
const configNs = getTimeseriesCollForDDLOps(
    st.s.getDB(dbName),
    st.s.getDB(dbName).getCollection(collName),
).getFullName();

// Find the tags doc. Zone ranges in config.tags use the internal "meta" field name.
let configDB = st.s.getDB("config");
let tags = configDB.tags.find({ns: configNs}).toArray();

assert.eq(1, tags.length, {tags});
assert.docEq({"meta.y": MinKey}, tags[0].min);
assert.docEq({"meta.y": 0}, tags[0].max);
assert.eq(existingZoneName, tags[0].tag);

// TODO (SERVER-127450): Re-enable once orphaned config.tags cleanup for mixed-version
// timeseries resharding is fixed.
// assert.eq(0, configDB.tags.find({ns: {$ne: configNs}}).itcount(), "stale orphaned tags found");

st.stop();
