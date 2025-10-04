/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_fcv_80
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {runTimeseriesRetryDeleteAndUpdateTest} from "jstests/libs/collection_write_path/timeseries_retry_delete_and_update.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
});

runTimeseriesRetryDeleteAndUpdateTest(
    st.s,
    function (db, coll, metaFieldName) {
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));
        assert.commandWorked(st.splitAt(getTimeseriesCollForDDLOps(db, coll).getFullName(), {meta: "B"}));
        assert.commandWorked(
            st.moveChunk(getTimeseriesCollForDDLOps(db, coll).getFullName(), {meta: "A"}, st.shard0.shardName),
        );
        assert.commandWorked(
            st.moveChunk(getTimeseriesCollForDDLOps(db, coll).getFullName(), {meta: "C"}, st.shard1.shardName),
        );
    },
    function (db, retriedCommandsCount, statementsRetried) {
        const transactionsServerStatusShard0 = st.shard0.getDB(db.getName()).serverStatus().transactions;
        const transactionsServerStatusShard1 = st.shard1.getDB(db.getName()).serverStatus().transactions;

        // On sharded clusters, retriedCommandsCount is the same as retriedStatementsCount because
        // each statement is executed on the shard individually as its own command.
        // We check that the actual server counts for these stats is at least the expected value, if
        // not more - it would be more if the server actually encountered aborted and retried
        // transactions outside of the ones we simulate in this test.
        assert.lte(
            retriedCommandsCount + statementsRetried,
            transactionsServerStatusShard0.retriedCommandsCount + transactionsServerStatusShard1.retriedCommandsCount,
            "Incorrect value for retriedCommandsCount in serverStatus, shard0: " +
                tojson(transactionsServerStatusShard0) +
                ", shard1: " +
                tojson(transactionsServerStatusShard1),
        );

        return statementsRetried;
    },
    function (db, retriedStatementsCount, statementsRetried) {
        const transactionsServerStatusShard0 = st.shard0.getDB(db.getName()).serverStatus().transactions;
        const transactionsServerStatusShard1 = st.shard1.getDB(db.getName()).serverStatus().transactions;

        assert.lte(
            retriedStatementsCount + statementsRetried,
            transactionsServerStatusShard0.retriedStatementsCount +
                transactionsServerStatusShard1.retriedStatementsCount,
            "Incorrect value for retriedStatementsCount in serverStatus, shard0: " +
                tojson(transactionsServerStatusShard0) +
                ", shard1: " +
                tojson(transactionsServerStatusShard1),
        );

        return statementsRetried;
    },
    st.shard0,
);

st.stop();
