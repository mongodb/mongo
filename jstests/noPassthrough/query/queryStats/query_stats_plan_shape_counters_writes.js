/**
 * Asserts that write commands never increment plan shape counters in query stats.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sharding,
 * ]
 */
import {it} from "jstests/libs/mochalite.js";
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {
    describeWriteCmdQueryStatsReplicaSetTests,
    describeWriteCmdQueryStatsShardedTests,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

function assertWriteCommandsDoNotCount(testDB, collName) {
    const statsConn = testDB.getMongo();

    // Run the write commands.
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{v: 5}]}));
    assert.commandWorked(
        testDB.runCommand({
            update: collName,
            updates: [{q: {v: {$gte: 0}}, u: {$set: {updated: true}}, multi: true}],
        }),
    );
    assert.commandWorked(testDB.runCommand({delete: collName, deletes: [{q: {v: 5}, limit: 0}]}));

    const stats = getQueryStats(statsConn, {collName});

    // Assert that we see the expected types of commands in query stats.
    const commands = stats.map((entry) => entry.key.queryShape.command).sort();
    assert.eq(["delete", "insert", "update"], commands, "expected one entry per write command", {
        stats,
    });

    // None of the query stats entries should have plan shape counters.
    for (const entry of stats) {
        const metrics = entry.metrics;
        const queryPlanner = metrics.hasOwnProperty("queryPlanner")
            ? metrics.queryPlanner
            : metrics;
        assert(
            !queryPlanner.hasOwnProperty("planShapeCounters"),
            "write command must not report plan shape counters",
            {entry},
        );
    }
}

describeWriteCmdQueryStatsReplicaSetTests("plan shape counters writes (replica set)", (ctxFn) => {
    it("write commands do not report plan shape counters", function () {
        const {testDB, collName} = ctxFn();
        assertWriteCommandsDoNotCount(testDB, collName);
    });
});

describeWriteCmdQueryStatsShardedTests("plan shape counters writes (sharded)", (ctxFn) => {
    it("write commands do not report plan shape counters", function () {
        const {testDB, collName} = ctxFn();
        assertWriteCommandsDoNotCount(testDB, collName);
    });
});
