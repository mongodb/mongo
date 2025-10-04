/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_fcv_80
 * ]
 */
import {runTimeseriesRetryDeleteAndUpdateTest} from "jstests/libs/collection_write_path/timeseries_retry_delete_and_update.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate();

runTimeseriesRetryDeleteAndUpdateTest(
    rst.getPrimary(),
    function (db, coll, metaFieldName) {},
    function (db, retriedCommandsCount, statementsRetried) {
        const transactionsServerStatus = db.serverStatus().transactions;
        assert.eq(
            retriedCommandsCount + 1,
            transactionsServerStatus.retriedCommandsCount,
            "Incorrect statistic in db.serverStatus(): " + tojson(transactionsServerStatus),
        );
        return 1;
    },
    function (db, retriedStatementsCount, statementsRetried) {
        const transactionsServerStatus = db.serverStatus().transactions;
        assert.eq(
            retriedStatementsCount + statementsRetried,
            transactionsServerStatus.retriedStatementsCount,
            "Incorrect statistic in db.serverStatus(): " + tojson(transactionsServerStatus),
        );
        return statementsRetried;
    },
);

rst.stopSet();
