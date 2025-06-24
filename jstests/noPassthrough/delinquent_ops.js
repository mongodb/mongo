/**
 * This file tests the server mechanism which determines and reports when an operation is marked
 * overdue on checking for interrupt. This does NOT aim to test that specific operations running
 * under normal conditions are not overdue.
 *
 * Whether an operation is overdue is determined by measuring wall clock time between interrupt
 * checks, so this test is inherently racey. To avoid flakiness, the test adjusts the threshold and
 * uses fail points to force operations to be overdue. This test should not be run on extremely
 * slow builds (e.g. code coverage, sanitizers), but is designed to be resistant to variations on
 * operation time for normal builds.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertNoOverdueOps(serverStatus) {
    const operationMetrics = serverStatus.metrics.operation;
    assert.eq(operationMetrics.overdueInterruptOps, 0, operationMetrics);
    assert.eq(operationMetrics.overdueInterruptChecks, 0, operationMetrics);
    assert.eq(operationMetrics.overdueInterruptTotalMillis, 0, operationMetrics);
    assert.eq(operationMetrics.overdueInterruptApproxMaxMillis, 0, operationMetrics);
}

function runTest(conn, failPointName) {
    const db = conn.getDB("test");
    const adminDB = db.getSiblingDB("admin");

    const isMongos = FixtureHelpers.isMongos(db);

    // After startup, we check that there were no operations marked overdue, using the extremely
    // high threshold that was configured earlier.
    {
        const serverStatus = db.serverStatus();
        const operationMetrics = serverStatus.metrics.operation;
        assert.gte(operationMetrics.totalInterruptChecks, 0, operationMetrics);

        assertNoOverdueOps(serverStatus);
    }

    // Run a ping() command that we don't expect to be overdue.
    {
        const pingResult = adminDB.runCommand({ping: 1});
        assert.commandWorked(pingResult, "Ping command failed");

        const serverStatus = db.serverStatus();
        const operationMetrics = serverStatus.metrics.operation;

        assert.gte(operationMetrics.totalInterruptChecks, 0, operationMetrics);

        assertNoOverdueOps(serverStatus);

        // TODO SERVER-104009: Once we have per-command information, we can also make an assertion
        // about the ping command not being overdue.
        const pingMetrics = serverStatus.metrics.commands.ping;
        assert.gte(pingMetrics.total, 1);
    }

    {
        // Now we run an operation that hangs and is overdue in checking for interrupt.

        if (!isMongos) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
        }
        // Before running the operation, reduce our threshold to 30ms. An operation that hangs for
        // 100ms should get marked overdue.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, overdueInterruptCheckIntervalMillis: 30}));
        const kSleepTimeMillis = 100;

        // Collect the number of overdue operations before running the op that we force to be
        // overdue.
        const previousOperationMetrics = db.serverStatus().metrics.operation;

        // Insert some data for the query to spin on.
        assert.commandWorked(db.testColl.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

        // We enable the failpoint, then in a parallel shell run a find() command that will hang.
        const failPoint = configureFailPoint(conn, failPointName);
        const joinShell = startParallelShell(function() {
            assert.eq(db.testColl.find().itcount(), 4);
        }, conn.port);
        jsTestLog("Waiting for fail point to be hit");
        failPoint.wait();
        jsTestLog("Sleeping while operation is blocked on failpoint");
        sleep(kSleepTimeMillis);
        jsTestLog("Disabling failpoint and waiting for shell");
        failPoint.off();
        joinShell();

        // Ensure that serverStatus indicates a find() was run.
        const serverStatus = db.serverStatus();
        const findMetrics = serverStatus.metrics.commands["find"];
        assert.gte(findMetrics.total, 1);

        const operationMetrics = serverStatus.metrics.operation;

        // Our hanging find() command should have bumped the counter. We cannot guarantee that any
        // other operations were _not_ overdue, so we simply assert that the number of
        // overdue ops has strictly increased.
        function errorString() {
            return "Operation metrics before overdue op: " + tojson(previousOperationMetrics) +
                " Most recent operation metrics " + tojson(operationMetrics);
        }
        assert.gt(operationMetrics.totalInterruptChecks,
                  previousOperationMetrics.totalInterruptChecks,
                  errorString);
        assert.gt(operationMetrics.overdueInterruptOps,
                  previousOperationMetrics.overdueInterruptOps,
                  errorString);
        assert.gt(operationMetrics.overdueInterruptChecks,
                  previousOperationMetrics.overdueInterruptChecks,
                  errorString);
        assert.gt(operationMetrics.overdueInterruptTotalMillis,
                  previousOperationMetrics.overdueInterruptTotalMillis,
                  errorString);
        assert.gte(
            operationMetrics.overdueInterruptApproxMaxMillis, kSleepTimeMillis / 2, errorString);

        // TODO SERVER-104009: Once we have per-command information, we can make a stronger
        // assertion about the find() command being overdue.
    }
}

// We start the server with a very high threshold. This is to avoid any internal operations which
// are considered overdue from polluting the overdue counters.  This test only focuses on the
// mechanism by which overdueness is determined and reported, and does not aim to enforce that all
// (or some fraction) of operations check for interrupt on time.
const startupParameters = {
    overdueInterruptCheckIntervalMillis: 100 * 1000
};

{
    let conn = MongoRunner.runMongod({setParameter: startupParameters});

    // Don't run this test on slow builds, as it can be racey.
    if (isSlowBuild(conn.getDB("test"))) {
        jsTestLog("Aborting test since it's running on a slow build");
        MongoRunner.stopMongod(conn);
        quit();
    }

    runTest(conn, "setYieldAllLocksHang");
    MongoRunner.stopMongod(conn);
}

{
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 1, setParameter: startupParameters},
        mongos: 1,
        mongosOptions: {setParameter: startupParameters}
    });
    // Use a different failpoint in the sharded version, since the mongos does not have a
    // setYieldAlllocksHang failpoint.
    runTest(st.s, "waitInFindBeforeMakingBatch");
    st.stop();
}
