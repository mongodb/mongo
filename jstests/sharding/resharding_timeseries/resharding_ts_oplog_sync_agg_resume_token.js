/**
 * Test that the postBatchResumeToken field is only included for the oplog namespace when
 * $_requestReshardingResumeToken is specified for an aggregate command, for timeseries collections.
 * Each document is inserted with a distinct sensorId to force a separate bucket entry in the
 * oplog. The oplog entries are then bucket-level operations on system.buckets.foo; we filter by
 * "o.meta.x" (the internal representation of the user-facing "sensorId.x" field in bucket
 * documents) to exercise partial-match PBRT scenarios.
 *
 * Timeseries variant of resharding_oplog_sync_agg_resume_token.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {runOplogSyncAggResumeTokenTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogSyncAggResumeTokenTest({
    setupCollection(testDB, collName) {
        // Create a timeseries collection with a non-"meta" metaField name to exercise the shard
        // key translation path (user-facing field -> internal bucket field).
        assert.commandWorked(
            testDB.createCollection(collName, {
                timeseries: {timeField: "ts", metaField: "sensorId"},
            }),
        );
    },
    makeDocument(i) {
        // Each document has a distinct sensorId to force one bucket per document in the oplog.
        // Bucket documents store the user-facing metaField value under the internal "meta" key,
        // so the oplog entry for sensorId {x: i} has "o.meta.x == i".
        return {ts: new Date(), sensorId: {x: i}, v: i};
    },
    oplogFilterField: "o.meta.x",
});
