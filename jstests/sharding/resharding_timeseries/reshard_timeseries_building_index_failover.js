/**
 * Tests that when timeseries resharding is in building-index phase, failover happens and resharding
 * should still work correctly. Uses a non-"meta" metaField name to exercise the shard key
 * translation path (user-facing field -> internal bucket field).
 *
 * Timeseries variant of resharding_building_index_failover.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {runBuildingIndexFailover} from "jstests/sharding/libs/resharding_failover_helpers.js";

runBuildingIndexFailover({
    // "sensorId.x" is the user-facing field; translated internally to {"meta.x": 1}.
    shardKeyPattern: {"sensorId.x": 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: {timeField: "ts", metaField: "sensorId"}},
    // "sensorId.y" is the user-facing field; translated internally to {"meta.y": 1}.
    newShardKeyPattern: {"sensorId.y": 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.y": MinKey}, max: {"meta.y": MaxKey}, shard: recipientShardNames[0]},
    ],
    documents: [
        {ts: new Date(), sensorId: {x: 1, y: -1}},
        {ts: new Date(), sensorId: {x: 2, y: -2}},
    ],
    indexKey: {"sensorId.x": 1},
    // getIndexes() returns user-facing field names for timeseries collections.
    newShardKeyIndexField: "sensorId.y",
});
