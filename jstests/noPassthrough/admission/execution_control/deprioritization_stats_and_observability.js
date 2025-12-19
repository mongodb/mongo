/**
 * Tests execution control observability features including algorithm reporting, deprioritized
 * operation statistics, and diagnostic information in serverStatus, $currentOp, and logs.
 *
 * @tags: [
 *   # The test deploys replica sets with execution control concurrency adjustment configured by
 *   # each test case, which should not be overwritten and expect to have 'throughputProbing' as
 *   # default.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getQueryExecMetrics, getQueryStats} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    getExecutionControlStats,
    getTotalDeprioritizationCount,
    insertTestDocuments,
    kFixedConcurrentTransactionsAlgorithm,
    kThroughputProbingAlgorithm,
    setBackgroundTaskDeprioritization,
    setDeprioritizationGate,
    setExecutionControlAlgorithm,
    setExecutionControlReadMaxQueueDepth,
    setExecutionControlReadLowPriorityMaxQueueDepth,
    setHeuristicDeprioritization,
    setHeuristicDeprioritizationThreshold,
} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

describe("Execution control statistics and observability", function () {
    function runNonDeprioritizedFind(coll) {
        return coll.find({_id: -1}).itcount();
    }

    function runDeprioritizedFind(coll, comment) {
        return coll.find().hint({$natural: 1}).comment(comment).batchSize(3).toArray();
    }

    describe("Execution control algorithm and prioritization reporting in serverStatus", function () {
        let replTest, mongod, db;
        const kNumReadTickets = 5;
        const kNumWriteTickets = 5;

        function verifyTicketAggregationStats(assertFunc, obj1, obj2) {
            // Step 1: ensure the statistic schema is correct.
            assert.eq(true, obj1.hasOwnProperty("available"));
            assert.eq(true, obj1.hasOwnProperty("out"));
            assert.eq(true, obj1.hasOwnProperty("totalTickets"));
            assert.eq(true, obj2.hasOwnProperty("available"));
            assert.eq(true, obj2.hasOwnProperty("out"));
            assert.eq(true, obj2.hasOwnProperty("totalTickets"));
            // Step 2: compare the statistics. The caller tries to assert compFunc(obj1, obj2).
            // Not comparing out because it depends on whether there is an op running or not.
            assertFunc(obj1.available, obj2.available);
            assertFunc(obj1.totalTickets, obj2.totalTickets);
        }

        /**
         * Verifies that execution control stats from serverStatus match the expected values.
         * Checks algorithm type, prioritization settings, and ticket aggregation stats.
         *
         * The 'expected' object should contain:
         * - usesThroughputProbing: whether throughput probing algorithm is active.
         * - usesPrioritization: whether any form of prioritization is enabled.
         * - deprioritizationGate: the state of the deprioritization gate.
         * - heuristicDeprioritization: whether heuristic deprioritization is enabled.
         * - backgroundTasksDeprioritization: whether background task deprioritization is enabled.
         * - ticketAssertFunc: assertion function (assert.eq or assert.gt) for comparing aggregated
         * ticket stats to normal priority ticket stats.
         */
        function verifyExecutionControlStats(expected) {
            const stats = getExecutionControlStats(mongod);
            assert.eq(expected.usesThroughputProbing, stats.usesThroughputProbing);
            assert.eq(expected.usesPrioritization, stats.usesPrioritization);
            assert.eq(expected.deprioritizationGate, stats.deprioritizationGate);
            assert.eq(expected.heuristicDeprioritization, stats.heuristicDeprioritization);
            assert.eq(expected.backgroundTasksDeprioritization, stats.backgroundTasksDeprioritization);
            verifyTicketAggregationStats(expected.ticketAssertFunc, stats.read, stats.read.normalPriority);
            verifyTicketAggregationStats(expected.ticketAssertFunc, stats.write, stats.write.normalPriority);
        }

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrentReadTransactions: kNumReadTickets,
                        executionControlConcurrentReadLowPriorityTransactions: kNumReadTickets,
                        executionControlConcurrentWriteTransactions: kNumWriteTickets,
                        executionControlConcurrentWriteLowPriorityTransactions: kNumWriteTickets,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
            db = mongod.getDB(jsTestName());
        });

        after(function () {
            replTest.stopSet();
        });

        it("should report throughput probing correctly", function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setDeprioritizationGate(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: true,
                usesPrioritization: false,
                deprioritizationGate: false,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.eq,
            });
        });

        it("should report fixed algorithm correctly", function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
            setDeprioritizationGate(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: false,
                usesPrioritization: false,
                deprioritizationGate: false,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.eq,
            });
        });

        it("should report throughput probing with prioritization correctly", function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setDeprioritizationGate(mongod, true);

            verifyExecutionControlStats({
                usesThroughputProbing: true,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report fixed algorithm with prioritization correctly", function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
            setDeprioritizationGate(mongod, true);

            verifyExecutionControlStats({
                usesThroughputProbing: false,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report heuristic deprioritization disabled correctly", function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: false,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: false,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report background tasks deprioritization disabled correctly", function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, true);
            setBackgroundTaskDeprioritization(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: false,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: false,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report both heuristic and background tasks disabled correctly", function () {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, false);
            setBackgroundTaskDeprioritization(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: false,
                usesPrioritization: false,
                deprioritizationGate: true,
                heuristicDeprioritization: false,
                backgroundTasksDeprioritization: false,
                ticketAssertFunc: assert.eq,
            });
        });

        it("should report prioritization with throughput probing and only heuristic enabled", function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, true);
            setBackgroundTaskDeprioritization(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: true,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: true,
                backgroundTasksDeprioritization: false,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report prioritization with throughput probing and only background tasks enabled", function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, false);
            setBackgroundTaskDeprioritization(mongod, true);

            verifyExecutionControlStats({
                usesThroughputProbing: true,
                usesPrioritization: true,
                deprioritizationGate: true,
                heuristicDeprioritization: false,
                backgroundTasksDeprioritization: true,
                ticketAssertFunc: assert.gt,
            });
        });

        it("should report no prioritization when gate is open but both flags are disabled", function () {
            setExecutionControlAlgorithm(mongod, kThroughputProbingAlgorithm);
            setDeprioritizationGate(mongod, true);
            setHeuristicDeprioritization(mongod, false);
            setBackgroundTaskDeprioritization(mongod, false);

            verifyExecutionControlStats({
                usesThroughputProbing: true,
                usesPrioritization: false,
                deprioritizationGate: true,
                heuristicDeprioritization: false,
                backgroundTasksDeprioritization: false,
                ticketAssertFunc: assert.eq,
            });
        });
    });

    describe("Deprioritized operation statistics and observability", function () {
        let replTest, mongod, db, coll;
        const kNumOpsThreshold = 5;
        const findComment = "deprioritized_find_test";

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        // Force the query to yield frequently to better expose the low-priority
                        // behavior.
                        internalQueryExecYieldIterations: 1,
                        executionControlDeprioritizationGate: true,
                        executionControlHeuristicNumAdmissionsDeprioritizeThreshold: 1,
                        logComponentVerbosity: {command: 2},
                    },
                    slowms: 0,
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
            db = mongod.getDB(jsTestName());
            coll = db.coll;

            insertTestDocuments(coll, 100);
        });

        after(function () {
            replTest.stopSet();
        });

        it("should correctly aggregate stats in serverStatus", function () {
            const beforeStats = getExecutionControlStats(mongod).read;

            // Run a fast, non-yielding query that will remain in the normal priority queue.
            runNonDeprioritizedFind(coll);
            const afterNormalStats = getExecutionControlStats(mongod).read;
            assert.gt(
                afterNormalStats.normalPriority.finishedProcessing,
                beforeStats.normalPriority.finishedProcessing,
            );

            // Run the de-prioritized query to completion.
            runDeprioritizedFind(coll, findComment + "_stats");

            // Check the server status again after the low-priority find has completed.
            const afterLowStats = getExecutionControlStats(mongod).read;

            // Verify low priority stats changes.
            assert.gte(afterLowStats.out, beforeStats.normalPriority.out);
            assert.gte(afterLowStats.available, beforeStats.normalPriority.available);
            assert.gte(afterLowStats.totalTickets, beforeStats.normalPriority.totalTickets);

            // Check the stats aggregate.
            assert.gte(afterLowStats.out, afterLowStats.normalPriority.out + afterLowStats.lowPriority.out);
            assert.gte(
                afterLowStats.available,
                afterLowStats.normalPriority.available + afterLowStats.lowPriority.available,
            );
            assert.gte(
                afterLowStats.totalTickets,
                afterLowStats.normalPriority.totalTickets + afterLowStats.lowPriority.totalTickets,
            );
        });

        it("should report 'priorityLowered: true' in $currentOp while active", function () {
            const currentOpComment = findComment + "_currentop";
            const failPoint = configureFailPoint(mongod, "setPreYieldWait", {
                waitForMillis: 200,
                comment: currentOpComment,
            });

            const joinShell = startParallelShell(
                funWithArgs(
                    (dbName, collName, comment) => {
                        db.getSiblingDB(dbName)
                            [collName].find()
                            .hint({$natural: 1})
                            .comment(comment)
                            .batchSize(3)
                            .toArray();
                    },
                    db.getName(),
                    coll.getName(),
                    currentOpComment,
                ),
                mongod.port,
            );

            failPoint.wait({timesEntered: 3});

            // Check $currentOp for the 'priorityLowered' flag while the operation is active.
            const curOpResult = db.currentOp({
                "command.comment": currentOpComment,
                "ns": coll.getFullName(),
                active: true,
            });
            assert.eq(1, curOpResult.inprog.length, tojson(curOpResult));
            assert.eq(true, curOpResult.inprog[0].priorityLowered);

            joinShell();
        });

        it("should report 'priorityLowered: true' in the slow query log", function () {
            const slowLogComment = findComment + "_slowlog";
            runDeprioritizedFind(coll, slowLogComment);

            const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
            const logLine = findMatchingLogLine(log, {msg: "Slow query", comment: slowLogComment});
            assert(logLine, `Could not find slow query log line for find with comment: ${slowLogComment}`);

            const parsedLog = JSON.parse(logLine);
            assert.eq(
                true,
                parsedLog.attr.priorityLowered,
                "Slow query log should have 'priorityLowered: true': " + logLine,
            );
        });

        it("should totalize deprioritized operations", function () {
            const beforeStats = getTotalDeprioritizationCount(mongod);

            // Verify normal ops don't affect total.
            runNonDeprioritizedFind(coll);
            assert.eq(beforeStats, getTotalDeprioritizationCount(mongod));

            // Verify low priority ops are totalized.
            runDeprioritizedFind(coll, findComment + "_totalization");
            assert.lt(beforeStats, getTotalDeprioritizationCount(mongod));
        });

        it("should report the heuristic threshold", function () {
            setHeuristicDeprioritizationThreshold(mongod, kNumOpsThreshold);
            const stats = getExecutionControlStats(mongod);
            assert(
                stats.hasOwnProperty("heuristicDeprioritizationThreshold"),
                `Missing heuristicDeprioritizationThreshold in execution stats: ` + tojson(stats),
            );
            assert.eq(
                kNumOpsThreshold,
                stats.heuristicDeprioritizationThreshold,
                "Heuristic deprioritization threshold not reported correctly in serverStatus",
            );
        });
    });
    describe("deprioritization reporting in query execution metrics", function () {
        let replTest, mongod, db, coll;
        const findComment = "deprioritized_find_test";

        before(function () {
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        // Force the query to yield frequently to better expose the low-priority
                        // behavior.
                        internalQueryExecYieldIterations: 1,
                        executionControlDeprioritizationGate: true,
                        executionControlHeuristicNumAdmissionsDeprioritizeThreshold: 1,
                        logComponentVerbosity: {command: 2},
                        internalQueryStatsRateLimit: -1,
                    },
                    slowms: 0,
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
            db = mongod.getDB(jsTestName());
            coll = db.coll;

            insertTestDocuments(coll, 100);
        });

        after(function () {
            replTest.stopSet();
        });

        // TODO SERVER-112150: Once all versions support QueryStatsMetricsSubsections feature flag, simply check metrics["queryExec"].
        function verifyQueryStatsMetrics(metrics) {
            let statsObj = metrics.hasOwnProperty("queryExec") ? metrics["queryExec"] : metrics;
            assert.gte(statsObj.totalTimeQueuedMicros.sum, 0);
            assert.gte(statsObj.totalAdmissions.sum, 0);
            assert.gte(statsObj.wasLoadShed.false, 0);
            assert.gte(statsObj.wasDeprioritized.false, 0);
        }

        it("should report deprioritization in query execution metrics", function () {
            runDeprioritizedFind(coll, findComment + "_totalization");
            const queryStats = getQueryStats(mongod, {collName: coll.getName()});
            assert(
                queryStats.length === 1,
                "Expected to find exactly one query stats entry for '" + coll.getName() + "' " + tojson(queryStats),
            );
            verifyQueryStatsMetrics(queryStats[0].metrics);
        });
    });

    describe("Delinquent operation statistics", function () {
        let replTest, mongod, db, coll;
        const findComment = "delinquent_ops_test";
        // The failpoint will wait for this long before yielding for every iteration.
        const waitPerIterationMs = 200;
        // This is how long we consider an operation as delinquent.
        const delinquentIntervalMs = waitPerIterationMs - 20;

        before(function () {
            // We start the server with a high threshold for delinquency. This is to avoid any
            // internal operations which are considered delinquent from polluting the delinquency
            // counters. This test only focuses on the mechanism by which delinquency is determined
            // and reported, and does not aim to enforce that all (or some fraction) of operations
            // are not delinquent. Also lower the threshold of parameters to generate low priority
            // operations.
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlDeprioritizationGate: true,
                        executionControlHeuristicNumAdmissionsDeprioritizeThreshold: 1,
                        internalQueryExecYieldIterations: 1,
                        featureFlagRecordDelinquentMetrics: true,
                        delinquentAcquisitionIntervalMillis: delinquentIntervalMs,
                        overdueInterruptCheckIntervalMillis: delinquentIntervalMs * 100,
                        overdueInterruptCheckSamplingRate: 1.0,
                        internalQueryStatsRateLimit: -1,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
            db = mongod.getDB(jsTestName());
            coll = db.coll;

            insertTestDocuments(coll, 3);
        });

        after(function () {
            replTest.stopSet();
        });

        it("should track delinquent acquisitions for low priority operations", function () {
            const beforeStats = getExecutionControlStats(mongod).read.lowPriority;

            const failPoint = configureFailPoint(mongod, "setPreYieldWait", {
                waitForMillis: waitPerIterationMs,
                comment: findComment,
            });

            runDeprioritizedFind(coll, findComment);
            failPoint.off();

            const afterStats = getExecutionControlStats(mongod).read.lowPriority;
            assert.gt(afterStats.totalDelinquentAcquisitions, beforeStats.totalDelinquentAcquisitions);
        });
    });

    describe("Short/long running operation statistics", function () {
        const dbName = jsTestName();
        const collName = "testcoll";

        const kNumDocs = 1000;
        const kNumReadTickets = 5;
        const kNumWriteTickets = 5;
        const kQueuedReaders = 20;
        const kBusyTimeMs = 2000;
        let replTest, mongod, db, coll;

        // Keys for per-acquisition stats (in read/write shortRunning/longRunning).
        const perAcquisitionKeys = [
            "totalTimeProcessingMicros",
            "totalTimeQueuedMicros",
            "totalAdmissions",
            "newAdmissions",
            "totalDelinquentAcquisitions",
            "totalAcquisitionDelinquencyMillis",
            "maxAcquisitionDelinquencyMillis",
        ];

        // Keys for finalized stats (in top-level shortRunning/longRunning).
        const finalizedStatsKeys = [
            "totalCPUUsageMicros",
            "totalElapsedTimeMicros",
            "newAdmissionsLoadShed",
            "totalCPUUsageLoadShed",
            "totalElapsedTimeMicrosLoadShed",
            "totalAdmissionsLoadShed",
            "totalQueuedTimeMicrosLoadShed",
        ];

        function assertPerAcquisitionStatsPresent(stats) {
            perAcquisitionKeys.forEach((key) => {
                assert(stats.hasOwnProperty(key), `Missing ${key} in per-acquisition stats: ` + tojson(stats));
            });
        }

        function assertFinalizedStatsPresent(stats) {
            finalizedStatsKeys.forEach((key) => {
                assert(stats.hasOwnProperty(key), `Missing ${key} in finalized stats: ` + tojson(stats));
            });
        }

        function assertExecutionStatsCorrect(executionStats) {
            assert.gte(
                executionStats.read.normalPriority.totalTimeQueuedMicros +
                    executionStats.read.lowPriority.totalTimeQueuedMicros,
                executionStats.read.shortRunning.totalTimeQueuedMicros +
                    executionStats.read.longRunning.totalTimeQueuedMicros,
                "Read totalTimeQueuedMicros mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.write.normalPriority.totalTimeQueuedMicros +
                    executionStats.write.lowPriority.totalTimeQueuedMicros,
                executionStats.write.shortRunning.totalTimeQueuedMicros +
                    executionStats.write.longRunning.totalTimeQueuedMicros,
                "Write totalTimeQueuedMicros mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.write.normalPriority.totalDelinquentAcquisitions +
                    executionStats.write.lowPriority.totalDelinquentAcquisitions,
                executionStats.write.shortRunning.totalDelinquentAcquisitions +
                    executionStats.write.longRunning.totalDelinquentAcquisitions,
                "Write totalDelinquentAcquisitions mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.write.normalPriority.totalAcquisitionDelinquencyMillis +
                    executionStats.write.lowPriority.totalAcquisitionDelinquencyMillis,
                executionStats.write.shortRunning.totalAcquisitionDelinquencyMillis +
                    executionStats.write.longRunning.totalAcquisitionDelinquencyMillis,
                "Write totalAcquisitionDelinquencyMillis mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.read.normalPriority.startedProcessing +
                    executionStats.read.lowPriority.startedProcessing +
                    executionStats.read.exempt.startedProcessing,
                executionStats.read.longRunning.totalAdmissions + executionStats.read.shortRunning.totalAdmissions,
                "Read startedProcessing mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.write.normalPriority.startedProcessing +
                    executionStats.write.lowPriority.startedProcessing +
                    executionStats.write.exempt.startedProcessing,
                executionStats.write.longRunning.totalAdmissions + executionStats.write.shortRunning.totalAdmissions,
                "Write startedProcessing mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.read.normalPriority.newAdmissions +
                    executionStats.read.lowPriority.newAdmissions +
                    executionStats.read.exempt.newAdmissions,
                executionStats.read.longRunning.newAdmissions + executionStats.read.shortRunning.newAdmissions,
                "Read newAdmissions mismatch: " + tojson(executionStats),
            );
            assert.gte(
                executionStats.write.normalPriority.newAdmissions +
                    executionStats.write.lowPriority.newAdmissions +
                    executionStats.write.exempt.newAdmissions,
                executionStats.write.longRunning.newAdmissions + executionStats.write.shortRunning.newAdmissions,
                "Write newAdmissions mismatch: " + tojson(executionStats),
            );
        }

        function assertExecutionShedStatsCorrect(executionStats) {
            assert.gt(
                executionStats.shortRunning.newAdmissionsLoadShed + executionStats.longRunning.newAdmissionsLoadShed,
                0,
                "newAdmissionsLoadShed mismatch: " + tojson(executionStats),
            );
            assert.gt(
                executionStats.shortRunning.totalElapsedTimeMicrosLoadShed +
                    executionStats.longRunning.totalElapsedTimeMicrosLoadShed,
                0,
                "totalElapsedTimeMicrosLoadShed mismatch: " + tojson(executionStats),
            );
        }

        function exhaustReadTicketsForLimitedTime() {
            let threads = [];
            jsTestLog("Starting " + kQueuedReaders + " readers for " + kBusyTimeMs + " ms");
            for (let i = 0; i < kQueuedReaders; i++) {
                threads.push(
                    new Thread(
                        (host, dbName, collName, kBusyTimeMs) => {
                            let mongo = new Mongo(host);
                            mongo.setSecondaryOk();
                            let db = mongo.getDB(dbName);
                            let startTime = Date.now();
                            do {
                                try {
                                    db[collName].aggregate([{"$count": "x"}]);
                                } catch (e) {
                                    // Ignore errors due to operation being shed.
                                    if (e.code == ErrorCodes.AdmissionQueueOverflow) {
                                        continue;
                                    }
                                    throw e;
                                }
                            } while (Date.now() - startTime < kBusyTimeMs);
                        },
                        mongod.host,
                        dbName,
                        collName,
                        kBusyTimeMs,
                    ),
                );
            }
            threads.forEach((thread) => thread.start());
            jsTestLog("Waiting for " + kQueuedReaders + " readers to finish");
            threads.forEach((thread) => thread.join());
        }

        before(function () {
            // We start the server with a low number of read and write tickets to make sure we eventually run out of available tickets.
            replTest = new ReplSetTest({
                nodes: 1,
                nodeOptions: {
                    setParameter: {
                        executionControlConcurrentReadTransactions: kNumReadTickets,
                        executionControlConcurrentWriteTransactions: kNumWriteTickets,
                        executionControlConcurrentReadLowPriorityTransactions: kNumReadTickets,
                        executionControlConcurrentWriteLowPriorityTransactions: kNumWriteTickets,
                        // Make yielding more common.
                        internalQueryExecYieldPeriodMS: 1,
                        internalQueryExecYieldIterations: 1,
                    },
                },
            });
            replTest.startSet();
            replTest.initiate();
            mongod = replTest.getPrimary();
            db = mongod.getDB(dbName);
            coll = db[collName];
        });

        after(function () {
            replTest.stopSet();
        });

        it("should report short/long running stats in serverStatus", function () {
            insertTestDocuments(coll, kNumDocs);

            let executionStats = db.serverStatus().queues.execution;

            // Check top-level shortRunning/longRunning finalized stats.
            assert(
                executionStats.hasOwnProperty("shortRunning"),
                "Missing top-level shortRunning stats: " + tojson(executionStats),
            );
            assertFinalizedStatsPresent(executionStats.shortRunning);
            assert(
                executionStats.hasOwnProperty("longRunning"),
                "Missing top-level longRunning stats: " + tojson(executionStats),
            );
            assertFinalizedStatsPresent(executionStats.longRunning);

            // Check per-acquisition stats in read shortRunning/longRunning.
            assert(
                executionStats.read.hasOwnProperty("shortRunning"),
                "Missing read shortRunning stats: " + tojson(executionStats.read),
            );
            assertPerAcquisitionStatsPresent(executionStats.read.shortRunning);
            assert(
                executionStats.read.hasOwnProperty("longRunning"),
                "Missing read longRunning stats: " + tojson(executionStats.read),
            );
            assertPerAcquisitionStatsPresent(executionStats.read.longRunning);

            // Check per-acquisition stats in write shortRunning/longRunning.
            assert(
                executionStats.write.hasOwnProperty("shortRunning"),
                "Missing write shortRunning stats: " + tojson(executionStats.write),
            );
            assertPerAcquisitionStatsPresent(executionStats.write.shortRunning);
            assert(
                executionStats.write.hasOwnProperty("longRunning"),
                "Missing write longRunning stats: " + tojson(executionStats.write),
            );
            assertPerAcquisitionStatsPresent(executionStats.write.longRunning);
        });

        it("should report admissions histogram in serverStatus", function () {
            let executionStats = db.serverStatus().queues.execution;
            assert(
                executionStats.hasOwnProperty("admissions"),
                "Missing admissions histogram: " + tojson(executionStats),
            );

            const histogram = executionStats.admissions;
            const expectedBuckets = [
                "1-2",
                "3-4",
                "5-8",
                "9-16",
                "17-32",
                "33-64",
                "65-128",
                "129-256",
                "257-512",
                "513-1024",
                "1025+",
            ];
            for (const bucket of expectedBuckets) {
                assert(histogram.hasOwnProperty(bucket), `Missing histogram bucket ${bucket}: ` + tojson(histogram));
            }
        });

        function configureExecutionControlState(enableDeprioritization, shedding) {
            setExecutionControlAlgorithm(mongod, kFixedConcurrentTransactionsAlgorithm);

            setDeprioritizationGate(mongod, enableDeprioritization);

            if (shedding) {
                setExecutionControlReadMaxQueueDepth(mongod, 0);
                setExecutionControlReadLowPriorityMaxQueueDepth(mongod, 0);
            }
        }

        it("Check that short/long running stats are correct without deprioritization", function () {
            configureExecutionControlState(false /*deprioritization*/, false /*shedding*/);
            exhaustReadTicketsForLimitedTime();
            assertExecutionStatsCorrect(db.serverStatus().queues.execution);
        });
        it("Check that short/long running stats are correct with deprioritization", function () {
            configureExecutionControlState(true /*deprioritization*/, false /*shedding*/);
            exhaustReadTicketsForLimitedTime();
            assertExecutionStatsCorrect(db.serverStatus().queues.execution);
        });
        it("Checks the shed operations are counted in the stats without deprioritization", function () {
            configureExecutionControlState(false /*deprioritization*/, true /*shedding*/);
            exhaustReadTicketsForLimitedTime();
            assertExecutionShedStatsCorrect(db.serverStatus().queues.execution);
        });
        it("Checks the shed operations are counted in the stats with deprioritization", function () {
            configureExecutionControlState(true /*deprioritization*/, true /*shedding*/);
            exhaustReadTicketsForLimitedTime();
            assertExecutionShedStatsCorrect(db.serverStatus().queues.execution);
        });
    });
});
