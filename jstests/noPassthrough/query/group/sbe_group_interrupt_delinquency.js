/**
 * Checks that scalar and block SBE group spill merging stays within the interrupt interval.
 *
 * @tags: [requires_persistence]
 */
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage, getEngine} from "jstests/libs/query/analyze_plan.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Leave headroom for spill-store stalls while an unchecked merge still exceeds the threshold.
const delinquentIntervalMs = 1000;
const sleepPerIterationMs = 50;
const numDocs = 60;
const pipeline = [{$group: {_id: "$k", c: {$sum: 1}}}];

describe("SBE spilled group merge interrupt delinquency", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagRecordDelinquentMetrics: true,
                featureFlagSbeFull: true,
                internalQueryFrameworkControl: "trySbeEngine",
                overdueInterruptCheckIntervalMillis: delinquentIntervalMs * 100,
                overdueInterruptCheckSamplingRate: 1.0,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1,
                internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtMost: 1,
                internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtLeast: 1,
            },
        });
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB(jsTestName());

        if (jsTest.options.inEvergreen && isSlowBuild(db)) {
            jsTest.log.info("Aborting timing-sensitive test on a slow build");
            MongoRunner.stopMongod(conn);
            quit();
        }

        assert.commandWorked(
            db.createCollection("block", {timeseries: {timeField: "t", metaField: "m"}}),
        );
        const docs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            k: 0,
            // Separate buckets provide multiple block partials for the same group key.
            m: i,
            t: new Date(0),
        }));
        assert.commandWorked(db.scalar.insertMany(docs));
        assert.commandWorked(db.block.insertMany(docs));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
            conn = null;
        }
    });

    for (const {collection, stageName} of [
        {collection: "scalar", stageName: "group"},
        {collection: "block", stageName: "block_group"},
    ]) {
        it(`${stageName} checks interrupts between spilled partials`, function () {
            const coll = db[collection];
            const explain = coll
                .explain("executionStats")
                .aggregate(pipeline, {allowDiskUse: true});
            assert.eq(getEngine(explain), "sbe", explain);
            const stage = getAggPlanStage(explain, stageName);
            assert.neq(stage, null, explain);
            assert(stage.usedDisk, explain);
            assert.gte(stage.spilledRecords, 40, explain);

            const comment = `${jsTestName()}-${stageName}`;
            let iterations;
            const results = runWithParamsAllNonConfigNodes(
                db,
                {overdueInterruptCheckIntervalMillis: delinquentIntervalMs},
                () =>
                    withFailPoint(
                        conn,
                        "sleepInSbeGroupSpillMergeLoop",
                        (failPoint) => {
                            const results = coll
                                .aggregate(pipeline, {allowDiskUse: true, comment})
                                .toArray();
                            const state = assert.commandWorked(
                                db.adminCommand({
                                    getParameter: 1,
                                    "failpoint.sleepInSbeGroupSpillMergeLoop": 1,
                                }),
                            )["failpoint.sleepInSbeGroupSpillMergeLoop"];
                            // The shouldFail guard and execute each count one failpoint entry.
                            iterations = (state.timesEntered - failPoint.timesEntered) / 2;
                            return results;
                        },
                        {data: {millis: sleepPerIterationMs}},
                    ),
            );

            assert.eq(results, [{_id: 0, c: numDocs}]);
            // Without checks, these sleeps alone exceed the delinquency threshold.
            assert.gte(iterations, 40);
            const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
            const line = findMatchingLogLine(globalLog.log, {
                msg: "Slow query",
                comment,
                command: "aggregate",
            });
            assert.neq(line, null, "aggregation slow query log line not found");
            const parsedLine = JSON.parse(line);
            assert.eq(parsedLine.attr.queryFramework, "sbe", {parsedLine});
            assert(parsedLine.attr.usedDisk, {parsedLine});
            assert.gte(parsedLine.attr.groupSpilledRecords, 40, {parsedLine});
            const delinquencyInfo = parsedLine.attr.delinquencyInfo;
            assert(
                !delinquencyInfo || !delinquencyInfo.overdueInterruptChecks,
                "spilled merge was overdue on checking interrupts",
                {parsedLine},
            );
            assert.gte(parsedLine.attr.numInterruptChecks, iterations, {parsedLine});
        });
    }
});
