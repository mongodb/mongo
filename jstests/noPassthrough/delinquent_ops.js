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
 *
 * This file also tests the operations that have long-duration ticket acquisitions (so called
 * delinquent) are reporting the delinquent stats correctly.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine, getMatchingLoglinesCount} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The failpoint will wait for this long before yielding for every iteration.
const waitPerIterationMs = 200;
// This is how long we consider an operation as delinquent.
const delinquentIntervalMs = waitPerIterationMs - 20;
const findComment = "delinquent_ops.js-COMMENT";

function assertDelinquentStats(metrics, count, msg, previousOperationMetrics) {
    if (count > 0) {
        assert(metrics, msg);
        let totalCount = metrics.totalDelinquentAcquisitions;
        let totalMillis = metrics.totalAcquisitionDelinquencyMillis;
        let maxMillis = metrics.maxAcquisitionDelinquencyMillis;
        if (previousOperationMetrics) {
            totalCount -= previousOperationMetrics.totalDelinquentAcquisitions;
            totalMillis -= previousOperationMetrics.totalAcquisitionDelinquencyMillis;
        }
        assert.gte(totalCount, count, metrics);
        assert.gte(totalMillis, waitPerIterationMs * count, metrics);
        assert.gte(maxMillis, waitPerIterationMs, metrics);
    } else if (metrics) {
        assert.eq(metrics.totalDelinquentAcquisitions, 0);
        assert.eq(metrics.totalAcquisitionDelinquencyMillis, 0);
        assert.eq(metrics.maxAcquisitionDelinquencyMillis, 0);
    }
}

function assertNoOverdueOps(operationMetrics, previousOperationMetrics) {
    assert.eq(operationMetrics.sampledOps, previousOperationMetrics.sampledOps, operationMetrics);
    assert.eq(operationMetrics.checksFromSample, previousOperationMetrics.checksFromSample, operationMetrics);
    assert.eq(operationMetrics.overdueOpsFromSample, previousOperationMetrics.overdueOpsFromSample, operationMetrics);
    assert.eq(
        operationMetrics.overdueChecksFromSample,
        previousOperationMetrics.overdueChecksFromSample,
        operationMetrics,
    );
    assert.eq(
        operationMetrics.overdueInterruptTotalMillisFromSample,
        previousOperationMetrics.overdueInterruptTotalMillisFromSample,
        operationMetrics,
    );
    assert.eq(
        operationMetrics.overdueInterruptApproxMaxMillisFromSample,
        previousOperationMetrics.overdueInterruptApproxMaxMillisFromSample,
        operationMetrics,
    );
}

function assertOverdueOps(operationMetrics, previousOperationMetrics) {
    const interruptMetrics = operationMetrics.interrupt;
    const previousInterruptMetrics = previousOperationMetrics.interrupt;

    function errorString() {
        return {metricsBefore: previousInterruptMetrics, metricsAfter: operationMetrics};
    }
    assert.gt(interruptMetrics.checksFromSample, previousInterruptMetrics.checksFromSample, errorString);
    assert.gt(interruptMetrics.overdueOpsFromSample, previousInterruptMetrics.overdueOpsFromSample, errorString);
    assert.gt(interruptMetrics.overdueChecksFromSample, previousInterruptMetrics.overdueChecksFromSample, errorString);
    assert.gt(
        interruptMetrics.overdueInterruptTotalMillisFromSample,
        previousInterruptMetrics.overdueInterruptTotalMillisFromSample,
        errorString,
    );
    assert.gte(
        interruptMetrics.overdueInterruptApproxMaxMillisFromSample,
        previousInterruptMetrics.overdueInterruptApproxMaxMillisFromSample,
        errorString,
    );
}

function assertOverdueOpsSlowlogAndCurop(operationMetrics, count) {
    assert.gte(operationMetrics.numInterruptChecks, count, operationMetrics);
    assert.gte(operationMetrics.delinquencyInfo.overdueInterruptChecks, count, operationMetrics);
    assert.gte(
        operationMetrics.delinquencyInfo.overdueInterruptTotalMillis,
        count * (waitPerIterationMs - delinquentIntervalMs),
        operationMetrics,
    );
    assert.gte(
        operationMetrics.delinquencyInfo.overdueInterruptApproxMaxMillis,
        waitPerIterationMs - delinquentIntervalMs,
        operationMetrics,
    );
}

function setOverdueThreshold(db, thresholdMs) {
    assert.commandFailedWithCode(
        db.adminCommand({setParameter: 1, overdueInterruptCheckIntervalMillis: 9}),
        ErrorCodes.BadValue,
    );

    assert.commandWorked(db.adminCommand({setParameter: 1, overdueInterruptCheckIntervalMillis: thresholdMs}));
}

function testDelinquencyOnRouter(routerDb) {
    // Before running the operation, reduce our threshold so that an operation that hangs for
    // 200ms should get marked overdue.
    setOverdueThreshold(routerDb, delinquentIntervalMs);

    // Collect the number of overdue operations before running the op that we force to be
    // overdue.
    const previousOperationMetrics = routerDb.serverStatus().metrics.operation;

    // Configure a failpoint to ensure the find() command hangs for a while and considered
    // delinquent.
    const failPoint = configureFailPoint(routerDb, "waitInFindBeforeMakingBatch", {
        sleepFor: waitPerIterationMs,
        comment: findComment,
    });

    assert.eq(routerDb.testColl.find().comment(findComment).itcount(), 4);

    // Ensure that serverStatus indicates a find() was run.
    {
        const serverStatus = routerDb.serverStatus();
        const findMetrics = serverStatus.metrics.commands["find"];
        assert.gte(findMetrics.total, 2);
    }

    const serverStatus = routerDb.serverStatus();
    assertOverdueOps(serverStatus.metrics.operation, previousOperationMetrics);

    failPoint.off();
}

function testDelinquencyOnShard(routerDb, shardDb) {
    // Before running the operation, reduce our threshold so that an operation that hangs for
    // 200ms should get marked overdue.
    setOverdueThreshold(shardDb, delinquentIntervalMs);

    // Collect the number of overdue operations before running the op that we force to be
    // overdue.
    const previousOperationMetrics = shardDb.serverStatus();

    // Configure a failpoint to wait some time before yielding, so that the ticket hold by find()
    // command is considered delinquent.
    const failPoint = configureFailPoint(shardDb, "setPreYieldWait", {
        waitForMillis: waitPerIterationMs,
        comment: findComment,
    });

    // Run the find() command in a parallel shell to retrieve the $currentOp information.
    const joinShell = startParallelShell(
        funWithArgs(
            function (dbName, findComment) {
                assert.eq(db.getSiblingDB(dbName).testColl.find().batchSize(3).comment(findComment).itcount(), 4);
            },
            routerDb.getName(),
            findComment,
        ),
        routerDb.getMongo().port,
    );

    failPoint.wait({timesEntered: 3});
    const curOp = shardDb.currentOp({"command.comment": findComment, "command.find": "testColl", "active": true});
    joinShell();

    // Ensure that serverStatus indicates a find() was run.Add commentMore actions
    {
        const serverStatus = routerDb.serverStatus();
        const findMetrics = serverStatus.metrics.commands["find"];
        assert.gte(findMetrics.total, 1);
    }

    // Check that the currentOp information has the expected delinquent information, the failpoint
    // was hit for 3rd time, that means we had at least 2 delinquent acquisitions (the 3rd ticket
    // not released yet).
    {
        assert(
            curOp.inprog.length === 1,
            "Expected to find exactly one active find() command with the comment " + findComment,
        );
        assertDelinquentStats(curOp.inprog[0].delinquencyInfo, 2, curOp.inprog[0]);
        assertOverdueOpsSlowlogAndCurop(curOp.inprog[0], 2);
    }

    // After the find() command, we check that the serverStatus has the delinquent stats
    // updated correctly. The find() command should have acquired the ticket 4 times, and each
    // acquisition should have been delinquent, since we configured the failpoint to wait for
    // 'waitPerIterationMs' milliseconds before yielding.
    {
        const serverStatus = shardDb.serverStatus();
        assert.gte(serverStatus.metrics.commands["find"].total, 1);
        assertDelinquentStats(
            serverStatus.queues.execution.read.normalPriority,
            4,
            serverStatus,
            previousOperationMetrics.queues.execution.read.normalPriority,
        );
    }

    // Now examine the log for this find() command and ensure it has information
    // about the delinquent acquisitions checks.
    {
        const assertLine = (line, count) => {
            jsTest.log.info("Found log line " + tojson(line));
            assert(line, globalLog);

            const parsedLine = JSON.parse(line);
            const delinquencyInfo = parsedLine.attr.delinquencyInfo;
            assertDelinquentStats(delinquencyInfo, count, line);
            assertOverdueOpsSlowlogAndCurop(parsedLine.attr, count);
        };

        const globalLog = assert.commandWorked(shardDb.adminCommand({getLog: "global"}));
        const lineFind = findMatchingLogLine(globalLog.log, {
            msg: "Slow query",
            comment: findComment,
            "command": "find",
        });
        assertLine(lineFind, 3);
        const lineGetMore = findMatchingLogLine(globalLog.log, {
            msg: "Slow query",
            comment: findComment,
            "command": "getMore",
        });
        assertLine(lineGetMore, 1);
    }

    {
        const queryStats = getQueryStats(routerDb.getMongo(), {collName: "testColl"});
        assert(
            queryStats.length === 1,
            "Expected to find exactly one query stats entry for 'testColl' " + tojson(queryStats),
        );
        assert.gte(queryStats[0].metrics.delinquentAcquisitions.sum, 4, tojson(queryStats));
        assert.gte(
            queryStats[0].metrics.totalAcquisitionDelinquencyMillis.sum,
            waitPerIterationMs * 4,
            tojson(queryStats),
        );
        assert.gte(queryStats[0].metrics.maxAcquisitionDelinquencyMillis.max, waitPerIterationMs, tojson(queryStats));

        // For first batch, numInterruptChecks >=4, time ~=600ms
        // For second batch, numInterruptChecks >=3, time~=400ms
        assert.gte(queryStats[0].metrics.numInterruptChecksPerSec.sum, 7, tojson(queryStats));
        assert.gte(
            queryStats[0].metrics.overdueInterruptApproxMaxMillis.max,
            waitPerIterationMs - delinquentIntervalMs,
            tojson(queryStats),
        );
    }

    {
        const serverStatus = shardDb.serverStatus();
        assertOverdueOps(serverStatus.metrics.operation, previousOperationMetrics.metrics.operation);
    }

    failPoint.off();

    // Run a multi-doc transaction with a long running operation. Assert that it does not get
    // marked delinquent or bump the delinquency serverStatus counters.
    {
        const session = routerDb.getMongo().startSession();
        const sessionDb = session.getDatabase(jsTestName());
        assert.commandWorked(sessionDb.txn_coll.insert({a: 1}));

        const findTxnComment = "find_in_txn";
        const sleepMillis = 10 * 1000;

        {
            session.startTransaction();
            assert.eq(
                sessionDb.txn_coll
                    .find({$where: `sleep(${sleepMillis}); return true;`})
                    .comment(findTxnComment)
                    .itcount(),
                1,
            );
            assert.commandWorked(sessionDb.txn_coll.insert({a: 2}));
            session.commitTransaction();
        }

        const globalLog = assert.commandWorked(shardDb.adminCommand({getLog: "global"}));
        const line = findMatchingLogLine(globalLog.log, {
            msg: "Slow query",
            comment: findTxnComment,
            "command": "find",
        });
        const parsedLine = JSON.parse(line);
        assert(
            !("delinquencyInfo" in parsedLine.attr) || !("delinquentAcquisitions" in parsedLine.attr.delinquencyInfo),
            parsedLine,
        );

        // Check that the server status counters were not bumped. Here we can only do a loose check.
        {
            const serverStatus = shardDb.serverStatus();
            const queues = serverStatus.queues.execution;

            // Ensure that the slow find() did not bump the queue-level counters. To do this, we
            // assert that the max delinquent value for each queue is less than the time this
            // operation slept. This assumes that no other background operation that the test
            // didn't trigger directly was delinquent for more than 'sleepMillis'.
            assert.lt(queues.write.normalPriority.maxAcquisitionDelinquencyMillis, sleepMillis, queues);
            assert.lt(queues.read.normalPriority.maxAcquisitionDelinquencyMillis, sleepMillis, queues);
        }

        // Ensure that the query stats for this operation do not indicate that it's delinquent.
        {
            const queryStats = getQueryStats(routerDb.getMongo(), {collName: "txn_coll"});
            assert(
                queryStats.length === 1,
                "Expected to find exactly one query stats entry for 'testColl' " + tojson(queryStats),
            );
            assert.eq(queryStats[0].metrics.delinquentAcquisitions.sum, 0, queryStats);
            assert.eq(queryStats[0].metrics.totalAcquisitionDelinquencyMillis.sum, 0, queryStats);
            assert.eq(queryStats[0].metrics.maxAcquisitionDelinquencyMillis.max, 0, queryStats);
        }
    }
}

// routerDb is the database on the router (mongos) when the cluster is sharded, otherwise it is the
// same as shardDb.
function runTest(routerDb, shardDb) {
    // After startup, we check that there were no operations marked delinquent, using the extremely
    // high threshold for delinquency that was configured earlier.
    {
        // const shardStatus = shardDb.serverStatus();
        // TODO: SERVER-107670 Add back this assertion after we find the root cause.
        // assertDelinquentStats(shardStatus.queues.execution.read.normalPriority, 0, shardStatus);
        // TODO: SERVER-104007 Add back this assertion
        // assertOverdueOps(shardStatus.metrics.operation, null);
        // const routerStatus = routerDb.serverStatus();
        // TODO: SERVER-104007 Add back this assertion
        // assertOverdueOps(routerStatus.metrics.operation, null);
    }

    // Run a ping() command that we don't expect to be overdue.Add commentMore actions
    {
        const previousOperationMetrics = routerDb.serverStatus().metrics.operation;

        const pingResult = routerDb.getSiblingDB("admin").runCommand({ping: 1});
        assert.commandWorked(pingResult, "Ping command failed");

        const serverStatus = routerDb.serverStatus();
        assertNoOverdueOps(serverStatus.metrics.operation, previousOperationMetrics);

        const pingMetrics = serverStatus.metrics.commands.ping;
        assert.gte(pingMetrics.total, 1);
    }

    assert.commandWorked(routerDb.testColl.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

    testDelinquencyOnShard(routerDb, shardDb);
    if (routerDb !== shardDb) {
        // If the router and shard are different, we also test the delinquency on the router.
        testDelinquencyOnRouter(routerDb);
    }
}

// We start the server with a high threshold for delinquency. This is to avoid any internal
// operations which are considered delinquent from polluting the delinquency counters.  This test
// only focuses on the mechanism by which delinquency is determined and reported, and does not aim
// to enforce that all (or some fraction) of operations are not delinquent.
const startupParameters = {
    featureFlagRecordDelinquentMetrics: true,
    delinquentAcquisitionIntervalMillis: delinquentIntervalMs,
    internalQueryStatsRateLimit: -1,

    overdueInterruptCheckIntervalMillis: delinquentIntervalMs * 100,
    overdueInterruptCheckSamplingRate: 1.0, // For this test we sample 100% of the time.
};

{
    const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: startupParameters}});
    rst.startSet();
    rst.initiate();
    const conn = rst.getPrimary();
    const db = conn.getDB(jsTestName());

    // Don't run this test on slow builds, as it can be racy, except for local test.
    if (jsTest.options.inEvergreen && isSlowBuild(db)) {
        jsTest.log.info("Aborting test since it's running on a slow build");
        rst.stopSet();
        quit();
    }

    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

    runTest(db, db);
    rst.stopSet();
}

{
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 1, setParameter: startupParameters},
        mongos: 1,
        mongosOptions: {setParameter: startupParameters},
    });

    assert.commandWorked(st.shard0.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    runTest(st.s.getDB(jsTestName()), st.shard0.getDB(jsTestName()));
    st.stop();
}
