/**
 * Tests that the resharding operation will fail if a recipient shard would have missed oplog
 * entries from a donor shard, for timeseries collections. Each large timeseries document is
 * inserted with a distinct sensorId to force a separate bucket entry in the oplog, so the 1MB
 * oplog fills with bucket-level writes rather than regular document inserts.
 *
 * Timeseries variant of resharding_oplog_sync_agg_assert_min_oplog.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {runOplogSyncAggAssertMinOplogTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogSyncAggAssertMinOplogTest({
    setupCollection(testDB, testColl) {
        // Create a timeseries collection with a non-"meta" metaField name to exercise the shard
        // key translation path (user-facing field -> internal bucket field).
        assert.commandWorked(
            testDB.createCollection("foo", {timeseries: {timeField: "ts", metaField: "sensorId"}}),
        );
    },
    findAnchorOplogEntry(localDb, testColl, lastBeforeTs) {
        // Find the first bucket oplog entry written by our inserts. In viewless timeseries
        // (FCV >= 9.0), bucket operations are written to the oplog using the user-facing namespace
        // (e.g. "test.foo"). In viewful timeseries (FCV < 9.0), they use the internal
        // "test.system.buckets.foo" namespace.
        const collNs = getTimeseriesCollForDDLOps(testColl.getDB(), testColl).getFullName();
        return localDb.oplog.rs.findOne({ns: collNs, ts: {$gt: lastBeforeTs}});
    },
    insertNextDoc(testColl, counter, longString) {
        // Each insert uses a distinct sensorId to force a new bucket (and thus a new oplog entry).
        return testColl.insert({ts: new Date(), sensorId: {x: counter}, longString});
    },
});
