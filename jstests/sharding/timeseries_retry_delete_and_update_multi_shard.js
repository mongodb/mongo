/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # Needed to run createUnsplittableCollection
 *   # TODO (SERVER-87625) Remove feature flag dependency.
 *   featureFlagAuthoritativeShardCollection,
 * ]
 */

import {
    runTimeseriesRetryDeleteAndUpdateTest
} from "jstests/libs/timeseries_retry_delete_and_update.js"

const st = new ShardingTest({
    shards: 2,
});

runTimeseriesRetryDeleteAndUpdateTest(
    st.s,
    function(db, coll, metaFieldName) {
        const bucketsColl = db["system.buckets." + coll.getName()];
        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));
        assert.commandWorked(st.splitAt(bucketsColl.getFullName(), {meta: "B"}));
        assert.commandWorked(
            st.moveChunk(bucketsColl.getFullName(), {meta: "A"}, st.shard0.shardName));
        assert.commandWorked(
            st.moveChunk(bucketsColl.getFullName(), {meta: "C"}, st.shard1.shardName));
    },
    function(db, retriedCommandsCount, statementsRetried) {
        const transactionsServerStatusShard0 =
            st.shard0.getDB(db.getName()).serverStatus().transactions;
        const transactionsServerStatusShard1 =
            st.shard1.getDB(db.getName()).serverStatus().transactions;

        // On sharded clusters, retriedCommandsCount is the same as retriedStatementsCount because
        // each statement is executed on the shard individually as its own command.
        assert.eq(retriedCommandsCount + statementsRetried,
                  transactionsServerStatusShard0.retriedCommandsCount +
                      transactionsServerStatusShard1.retriedCommandsCount,
                  'Incorrect value for retriedCommandsCount in serverStatus, shard0: ' +
                      tojson(transactionsServerStatusShard0) +
                      ', shard1: ' + tojson(transactionsServerStatusShard1));

        return statementsRetried;
    },
    function(db, retriedStatementsCount, statementsRetried) {
        const transactionsServerStatusShard0 =
            st.shard0.getDB(db.getName()).serverStatus().transactions;
        const transactionsServerStatusShard1 =
            st.shard1.getDB(db.getName()).serverStatus().transactions;

        assert.eq(retriedStatementsCount + statementsRetried,
                  transactionsServerStatusShard0.retriedStatementsCount +
                      transactionsServerStatusShard1.retriedStatementsCount,
                  'Incorrect value for retriedStatementsCount in serverStatus, shard0: ' +
                      tojson(transactionsServerStatusShard0) +
                      ', shard1: ' + tojson(transactionsServerStatusShard1));

        return statementsRetried;
    });

st.stop();
