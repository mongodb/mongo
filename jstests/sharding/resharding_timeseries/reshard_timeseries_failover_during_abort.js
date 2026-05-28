/**
 * Simulates a failover prior to removing the recipient doc while timeseries resharding is aborting
 * from an unrecoverable error on the donor. Resharding should abort successfully after stepUp.
 * Uses a non-"meta" metaField name to exercise the shard key translation path
 * (user-facing field -> internal bucket field).
 *
 * Timeseries variant of resharding_failover_during_abort.js.
 *
 * See BF-32038 for more details.
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {runFailoverDuringAbort} from "jstests/sharding/libs/resharding_failover_helpers.js";

runFailoverDuringAbort({
    // "sensorId.x" is the user-facing field; translated internally to {"meta.x": 1}.
    shardKeyPattern: {"sensorId.x": 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.x": MinKey}, max: {"meta.x": 10}, shard: donorShardNames[0]},
        {min: {"meta.x": 10}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: {timeField: "ts", metaField: "sensorId"}},
    // "sensorId.y" is the user-facing field; translated internally to {"meta.y": 1}.
    newShardKeyPattern: {"sensorId.y": 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {"meta.y": MinKey}, max: {"meta.y": 10}, shard: recipientShardNames[0]},
        {min: {"meta.y": 10}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
    ],
    getRecipientDocNs: (coll) => getTimeseriesCollForDDLOps(coll.getDB(), coll).getFullName(),
});
