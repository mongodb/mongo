/**
 * Checks that classic $group spill merging checks interrupts between partials.
 * Per-partial sleeps exceed the overdue threshold when combined without checks.
 */

import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Allow scheduling delays while keeping an unchecked merge above the threshold.
const delinquentIntervalMs = 250;
const sleepPerIterationMs = 10;
const numKeys = 10;
const dupsPerKey = 40;
const aggComment = "group_interrupt_delinquency.js-COMMENT";

describe("$group spilled merge loop interrupt delinquency", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagRecordDelinquentMetrics: true,
                // Start with a high threshold so internal operations are not counted as
                // delinquent; the real threshold is set at runtime below.
                overdueInterruptCheckIntervalMillis: delinquentIntervalMs * 100,
                overdueInterruptCheckSamplingRate: 1.0,
                // Keep $group in GroupProcessor rather than pushing it down to SBE.
                internalQueryFrameworkControl: "forceClassicEngine",
            },
        });
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB(jsTestName());

        // Don't run this test on slow builds, as it is timing sensitive.
        if (jsTest.options.inEvergreen && isSlowBuild(db)) {
            jsTest.log.info("Aborting test since it's running on a slow build");
            MongoRunner.stopMongod(conn);
            quit();
        }

        assert.commandWorked(
            db.coll.insertMany(
                Array.from({length: numKeys * dupsPerKey}, (_, i) => ({
                    _id: i,
                    k: "key" + (i % numKeys),
                })),
            ),
        );
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
            conn = null;
        }
    });

    it("checks for interrupt on every spilled merge loop iteration", function () {
        // Spill on every duplicate id (capped at 20 files) so each key has ~20 spilled partials
        // to merge without needing a memory-limit's worth of data.
        const results = runWithParamsAllNonConfigNodes(
            db,
            {
                overdueInterruptCheckIntervalMillis: delinquentIntervalMs,
                internalQueryEnableAggressiveSpillsInGroup: true,
            },
            () =>
                withFailPoint(
                    conn,
                    "sleepInGroupSpillMergeLoop",
                    () =>
                        db.coll
                            .aggregate([{$group: {_id: "$k", c: {$sum: 1}}}], {
                                allowDiskUse: true,
                                comment: aggComment,
                            })
                            .toArray(),
                    {data: {millis: sleepPerIterationMs}},
                ),
        );

        assert.eq(results.length, numKeys, {results});
        for (const result of results) {
            assert.eq(result.c, dupsPerKey, {results});
        }

        // Check this aggregation's delinquency and interrupt counts.
        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        const line = findMatchingLogLine(globalLog.log, {
            msg: "Slow query",
            comment: aggComment,
            "command": "aggregate",
        });
        assert.neq(line, null, "slow query log line for the aggregation not found");
        const parsedLine = JSON.parse(line);
        jsTest.log.info("aggregation slow query log line", {parsedLine});
        const delinquencyInfo = parsedLine.attr.delinquencyInfo;
        assert(
            !delinquencyInfo || !delinquencyInfo.overdueInterruptChecks,
            "aggregation was overdue on checking interrupts",
            {parsedLine},
        );
        assert.gte(
            parsedLine.attr.numInterruptChecks,
            numKeys * (dupsPerKey / 4),
            "merge loop did not check for interrupt per iteration",
            {parsedLine},
        );
    });
});
