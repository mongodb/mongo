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

import {isSlowBuild} from "jstests/libs/aggregation_pipeline_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getQueryStats} from "jstests/libs/query_stats_utils.js";

// The failpoint will wait for this long before yielding for every iteration.
const waitPerIterationMs = 200;
// This is how long we consider an operation as delinquent.
const findComment = "delinquent_ops.js-COMMENT";

function runTest(routerDb, shardDb) {
    assert.commandWorked(routerDb.testColl.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));

    // Configure a failpoint to wait some time before yielding, so that the ticket hold by find()
    // command is considered delinquent.
    const failPoint = configureFailPoint(shardDb, "setYieldAllLocksWait", {
        waitForMillis: waitPerIterationMs,
        namespace: routerDb.testColl.getFullName(),
    });

    // Run the find() command in a parallel shell to retrieve the $currentOp information.
    const joinShell = startParallelShell(
        funWithArgs(
            function(dbName, findComment) {
                assert.eq(db.getSiblingDB(dbName)
                              .testColl.find()
                              .batchSize(3)
                              .comment(findComment)
                              .itcount(),
                          4);
            },
            routerDb.getName(),
            findComment,
            ),
        routerDb.getMongo().port,
    );

    failPoint.wait({timesEntered: 3});
    const curOp = shardDb.currentOp(
        {"command.comment": findComment, "command.find": "testColl", "active": true});
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
        assert.gte(curOp.inprog[0].numInterruptChecks, 2, curOp.inprog[0]);
    }

    // Now examine the log for this find() command and ensure it has information
    // about the delinquent acquisitions checks.
    {
        const assertLine = (line, count) => {
            jsTestLog("Found log line " + tojson(line));
            assert(line, globalLog);

            const parsedLine = JSON.parse(line);
            assert.gte(parsedLine.attr.numInterruptChecks, count, parsedLine.attr);
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

        // For first batch, numInterruptChecks >=4, time ~=600ms
        // For second batch, numInterruptChecks >=3, time~=400ms
        assert.gte(queryStats[0].metrics.numInterruptChecksPerSec.sum, 7, tojson(queryStats));
    }

    failPoint.off();
}

const startupParameters = {
    internalQueryStatsRateLimit: -1,
};
{
    const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: startupParameters}});
    rst.startSet();
    rst.initiate();
    const conn = rst.getPrimary();
    const db = conn.getDB(jsTestName());

    // Don't run this test on slow builds, as it can be racy, except for local test.
    if (jsTest.options.inEvergreen && isSlowBuild(db)) {
        jsTestLog("Aborting test since it's running on a slow build");
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

    assert.commandWorked(
        st.shard0.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    runTest(st.s.getDB(jsTestName()), st.shard0.getDB(jsTestName()));
    st.stop();
}