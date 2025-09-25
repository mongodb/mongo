/**
 * Ensures a delete on a sharded time-series collection is rejected when it would change the
 * owning shard by shifting a bucket's control.min.time across chunk boundaries.
 *
 * - Shard key: { time: 1 } (maps to buckets' {"control.min.time": 1}).
 * - Create two chunks: (-inf, t30) on shard0 and [t30, +inf) on shard1.
 * - Insert two measurements pre-sharding at t20 and t40 so they land in the same bucket whose
 *   min time is t20 (thus owned by shard0 after sharding/split).
 * - Attempt deleteOne({time: t20}). Removing t20 would make the bucket min become t40 (>= t30),
 *   implying the mutated bucket would belong to shard1. Operation must fail with
 *   ErrorCodes.WouldChangeOwningShard.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_83,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(function () {
    const dbName = jsTestName();
    const collName = "ts";
    const timeField = "time";

    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
    const mongos = st.s0;
    const db = mongos.getDB(dbName);

    try {
        assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField}}));
        const coll = db.getCollection(collName);

        function minutes(m) {
            return new Date(60 * 1000 * m);
        }
        const t20 = minutes(20);
        const t30 = minutes(30);
        const t40 = minutes(40);

        // Insert two measurements BEFORE sharding so they are guaranteed to be in the same bucket
        // (seconds granularity -> 1 hour bucket span; both t20 and t40 fit in one bucket).
        assert.commandWorked(coll.insertMany([{[timeField]: t20}, {[timeField]: t40}]));

        // Shard on the time field. For time-series, {time: 1} corresponds to buckets'
        // {"control.min.time": 1} routing.
        assert.commandWorked(coll.createIndex({[timeField]: 1}));
        assert.commandWorked(mongos.adminCommand({shardCollection: `${dbName}.${collName}`, key: {[timeField]: 1}}));

        // Split at t30 on the buckets collection and move the upper chunk to the other shard.
        const bucketsNss = getTimeseriesCollForDDLOps(db, coll).getFullName();
        const splitPoint = {[`control.min.${timeField}`]: t30};

        assert.commandWorked(mongos.adminCommand({split: bucketsNss, middle: splitPoint}));

        // Ensure both chunks are currently on the primary shard, then move one.
        let counts = st.chunkCounts(collName, dbName);
        const primary = st.getPrimaryShard(dbName);
        assert.eq(2, counts[primary.shardName], counts);

        const other = st.getOther(primary);
        assert.commandWorked(
            mongos.adminCommand({movechunk: bucketsNss, find: splitPoint, to: other.name, _waitForDelete: true}),
        );

        // Verify chunk distribution is 1 per shard.
        counts = st.chunkCounts(collName, dbName);
        assert.eq(1, counts[primary.shardName], counts);
        assert.eq(1, counts[other.shardName], counts);

        // Now attempt to delete the earlier measurement. This would change the bucket min from t20
        // to t40, crossing the chunk boundary and hence changing the owning shard. Expect failure.
        assert.commandFailedWithCode(
            db.runCommand({delete: collName, deletes: [{q: {[timeField]: t20}, limit: 1}]}),
            ErrorCodes.WouldChangeOwningShard,
        );

        // Ensure no documents were deleted.
        assert.eq(2, coll.countDocuments({}));
    } finally {
        st.stop();
    }
})();
