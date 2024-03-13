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
    shards: 1,
});

runTimeseriesRetryDeleteAndUpdateTest(
    st.s,
    function(db, coll, metaFieldName) {
        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));
    },
    function(db, retriedCommandsCount, statementsRetried) {
        const transactionsServerStatus = st.shard0.getDB(db.getName()).serverStatus().transactions;

        // On sharded clusters, retriedCommandsCount is the same as retriedStatementsCount because
        // each statement is executed on the shard individually as its own command.
        assert.eq(retriedCommandsCount + statementsRetried,
                  transactionsServerStatus.retriedCommandsCount,
                  'Incorrect value for retriedCommandsCount in serverStatus: ' +
                      tojson(transactionsServerStatus));

        return statementsRetried;
    },
    function(db, retriedStatementsCount, statementsRetried) {
        const transactionsServerStatus = st.shard0.getDB(db.getName()).serverStatus().transactions;

        assert.eq(retriedStatementsCount + statementsRetried,
                  transactionsServerStatus.retriedStatementsCount,
                  'Incorrect value for retriedStatementsCount in serverStatus:' +
                      tojson(transactionsServerStatus));

        return statementsRetried;
    });

st.stop();
