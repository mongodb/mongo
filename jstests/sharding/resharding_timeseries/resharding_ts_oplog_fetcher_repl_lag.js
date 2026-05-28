/**
 * Tests that resharding can handle the case where there is replication lag on donor shards when
 * resharding timeseries collections. The resharding oplog fetcher starts targeting the primary
 * node of the donor shard instead of the nearest node when the recipient is approaching strict
 * consistency, and must correctly set rawData when fetching oplogs from timeseries source
 * collections.
 *
 * Timeseries variant of resharding_oplog_fetcher_repl_lag.js.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_fcv_80,
 *   multiversion_incompatible,
 * ]
 */
import {runOplogFetcherReplLagTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogFetcherReplLagTest({
    setupCollection(st, dbName, collName) {
        // Create a timeseries collection with a non-"meta" metaField name to exercise the shard
        // key translation path (user-facing field -> internal bucket field).
        assert.commandWorked(
            st.s.getDB(dbName).createCollection(collName, {
                timeseries: {timeField: "ts", metaField: "sensorId"},
            }),
        );
        const testColl = st.s.getCollection(dbName + "." + collName);
        assert.commandWorked(
            testColl.insert([
                {ts: new Date(), sensorId: {x: -1}, v: 1},
                {ts: new Date(), sensorId: {x: 0}, v: 2},
                {ts: new Date(), sensorId: {x: 1}, v: 3},
            ]),
        );
    },
});
