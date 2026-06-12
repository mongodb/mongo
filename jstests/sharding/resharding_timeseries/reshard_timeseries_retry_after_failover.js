/**
 * Tests that if a timeseries reshardCollection command with a user-provided reshardingUUID is
 * completed, then after failover the result is available to retries. Uses a non-"meta" metaField
 * name to exercise the shard key translation path (user-facing field -> internal bucket field).
 *
 * Timeseries variant of reshard_collection_retry_after_failover.js.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_fcv_80,
 *   uses_atclustertime,
 * ]
 */

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {runRetryAfterFailover} from "jstests/sharding/libs/resharding_failover_helpers.js";

runRetryAfterFailover({
    // "sensorId.x" is the user-facing field; translated internally to {"meta.x": 1}.
    shardKeyPattern: {"sensorId.x": 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.x": MinKey}, max: {"meta.x": MaxKey}, shard: donorShardNames[0]},
    ],
    shardCollOptions: {timeseries: {timeField: "ts", metaField: "sensorId"}},
    // "sensorId.y" is the user-facing field; translated internally to {"meta.y": 1}.
    newShardKeyPattern: {"sensorId.y": 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.y": MinKey}, max: {"meta.y": MaxKey}, shard: recipientShardNames[0]},
    ],
    getAbortNs: (coll) => getTimeseriesCollForDDLOps(coll.getDB(), coll).getFullName(),
    getCollUUID: (db, coll) =>
        getUUIDFromListCollections(db, getTimeseriesCollForDDLOps(db, coll).getName()),
});
