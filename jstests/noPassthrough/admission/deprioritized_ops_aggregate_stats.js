/**
 * Checks that if an operation is de-prioritized, its stats are aggregated correctly in
 * serverStatus, and it is reported as low-priority in both $currentOp and the slow query log.
 *
 * @tags: [
 *   featureFlagMultipleTicketPoolsExecutionControl,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("Deprioritized operation diagnostics", function () {
    let rst;
    let primary;
    let db;
    let coll;
    const findComment = "deprioritized_find_for_curop_and_slowlog_test";

    const runDeprioritizedFind = function (dbName, collName, findComment) {
        db.getSiblingDB(dbName).getCollection(collName).find().hint({$natural: 1}).comment(findComment).toArray();
    };

    beforeEach(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    // Force the query to yield frequently to better expose the low-priority
                    // behavior.
                    internalQueryExecYieldIterations: 1,
                    storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactionsWithPrioritization",
                    storageEngineHeuristicDeprioritizationEnabled: true,
                    storageEngineHeuristicNumYieldsDeprioritizeThreshold: 1,
                    logComponentVerbosity: {command: 2},
                },
                slowms: 0,
            },
        });

        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        db = primary.getDB(jsTestName());
        coll = db.coll;

        assert.commandWorked(coll.insertMany([...Array(100).keys()].map((i) => ({_id: i}))));
    });

    afterEach(function () {
        rst.stopSet();
    });

    it("should correctly aggregate stats in serverStatus", function () {
        const beforeFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;

        // Run a fast, non-yielding query that will remain in the normal priority queue.
        coll.find({_id: -1}).itcount();
        const afterNormalFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;
        assert.gt(
            afterNormalFindStats.normalPriority.finishedProcessing,
            beforeFindStats.normalPriority.finishedProcessing,
        );

        // Run the de-prioritized query to completion.
        runDeprioritizedFind(db.getName(), coll.getName(), findComment);

        // Check the server status again after the low-priority find has completed.
        const afterLowFindStats = primary.adminCommand({serverStatus: 1}).queues.execution.read;

        // Verify low priority stats changes.
        assert.gte(afterLowFindStats.out, beforeFindStats.normalPriority.out);
        assert.gte(afterLowFindStats.available, beforeFindStats.normalPriority.available);
        assert.gte(afterLowFindStats.totalTickets, beforeFindStats.normalPriority.totalTickets);

        // Check the stats aggregate.
        assert.gte(afterLowFindStats.out, afterLowFindStats.normalPriority.out + afterLowFindStats.lowPriority.out);
        assert.gte(
            afterLowFindStats.available,
            afterLowFindStats.normalPriority.available + afterLowFindStats.lowPriority.available,
        );
        assert.gte(
            afterLowFindStats.totalTickets,
            afterLowFindStats.normalPriority.totalTickets + afterLowFindStats.lowPriority.totalTickets,
        );
    });

    it("should report 'priorityLowered: true' in $currentOp while active", function () {
        const failPoint = configureFailPoint(primary, "setPreYieldWait", {waitForMillis: 200, comment: findComment});

        const joinShell = startParallelShell(
            funWithArgs(runDeprioritizedFind, db.getName(), coll.getName(), findComment),
            primary.port,
        );

        failPoint.wait({timesEntered: 3});

        // Check $currentOp for the 'priorityLowered' flag while the operation is active.
        const curOpResult = db.currentOp({"command.comment": findComment, "ns": coll.getFullName(), active: true});
        assert.eq(1, curOpResult.inprog.length, tojson(curOpResult));
        assert.eq(true, curOpResult.inprog[0].priorityLowered);

        joinShell();
    });

    it("should report 'priorityLowered: true' in the slow query log", function () {
        runDeprioritizedFind(db.getName(), coll.getName(), findComment);

        const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        const logLine = findMatchingLogLine(log, {msg: "Slow query", comment: findComment});
        assert(logLine, `Could not find slow query log line for find with comment: ${findComment}`);

        const parsedLog = JSON.parse(logLine);
        assert.eq(
            true,
            parsedLog.attr.priorityLowered,
            "Slow query log should have 'priorityLowered: true': " + logLine,
        );
    });
});
