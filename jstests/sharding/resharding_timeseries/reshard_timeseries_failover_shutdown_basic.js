/**
 * Tests that timeseries reshardCollection succeeds when a participant experiences a failover or
 * clean/unclean restart during the operation. Uses a non-"meta" metaField name to exercise the
 * shard key translation path (user-facing field -> internal bucket field).
 *
 * Timeseries variant of reshard_collection_failover_shutdown_basic.js.
 *
 * Multiversion testing does not support tests that kill and restart nodes. So we had to add the
 * 'multiversion_incompatible' tag.
 * @tags: [
 *   uses_atclustertime,
 *   multiversion_incompatible,
 *   requires_persistence,
 *   requires_fcv_80,
 * ]
 */
import {runFailoverShutdownBasic} from "jstests/sharding/libs/resharding_failover_helpers.js";

runFailoverShutdownBasic({
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
        {min: {"meta.y": MinKey}, max: {"meta.y": 0}, shard: recipientShardNames[0]},
        {min: {"meta.y": 0}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
    ],
    documents: [
        {ts: new Date(), sensorId: {x: -10, y: -10}},
        {ts: new Date(), sensorId: {x: 10, y: -10}},
        {ts: new Date(), sensorId: {x: -10, y: 10}},
        {ts: new Date(), sensorId: {x: 10, y: 10}},
    ],
});
