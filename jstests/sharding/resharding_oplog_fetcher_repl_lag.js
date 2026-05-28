/**
 * Tests that resharding can handle the case where there is replication lag on donor shards. That
 * is, when the recipient is approaching strict consistency, the resharding oplog fetcher starts
 * targeting the primary node of the donor shard instead of the nearest node to prepare for the
 * critical section.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {runOplogFetcherReplLagTest} from "jstests/sharding/libs/resharding_oplog_helpers.js";

runOplogFetcherReplLagTest({
    setupCollection(st, dbName, collName) {
        const testColl = st.s.getCollection(dbName + "." + collName);
        assert.commandWorked(testColl.insert([{x: -1}, {x: 0}, {x: 1}]));
    },
});
