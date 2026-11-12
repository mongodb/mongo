/**
 * Detached hash-lookup cursors can be disposed by killCursors with or without a pipeline wrapper.
 * @tags: [requires_persistence]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

describe("detached hash lookup disposal", function () {
    let conn;
    let testDB;

    before(function () {
        conn = MongoRunner.runMongod({
            slowms: 0,
            setParameter: {
                featureFlagSbeFull: true,
                internalQueryFrameworkControl: "trySbeEngine",
                overdueInterruptCheckSamplingRate: 1.0,
                internalDocumentSourceCursorInitialBatchSize: 1,
                internalDocumentSourceCursorBatchSizeBytes: 1,
            },
        });
        testDB = conn.getDB(jsTestName());
        assert.commandWorked(testDB.local.insertMany([{k: -1}, {k: -1}, {k: -1}]));
        assert.commandWorked(
            testDB.foreign.insertMany(
                Array.from({length: 100}, (_, k) => ({k, payload: "x".repeat(1024)})),
            ),
        );
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    for (const requiresPipelineWrapper of [false, true]) {
        it(`disposes hash lookup with pipeline wrapper=${requiresPipelineWrapper}`, function () {
            const pipeline = [
                {
                    $lookup: {
                        from: "foreign",
                        localField: "k",
                        foreignField: "k",
                        as: "matches",
                    },
                },
            ];
            if (requiresPipelineWrapper) {
                // Prevent a fully pushed-down pipeline so disposal uses PlanExecutorPipeline.
                pipeline.push({$_internalInhibitOptimization: {}});
            }
            const explain = testDB.local.explain().aggregate(pipeline, {allowDiskUse: true});
            const lookup = getAggPlanStage(explain, "EQ_LOOKUP");
            assert.neq(lookup, null, {explain});
            assert.eq(lookup.strategy, "HashJoin", {explain});
            const cursor = assert.commandWorked(
                testDB.runCommand({
                    aggregate: "local",
                    pipeline,
                    allowDiskUse: true,
                    cursor: {batchSize: 1},
                }),
            ).cursor;
            assert.neq(cursor.id, 0);
            const comment = `${jsTestName()}-${requiresPipelineWrapper}`;
            const result = assert.commandWorked(
                testDB.runCommand({
                    killCursors: "local",
                    cursors: [cursor.id],
                    comment,
                }),
            );
            assert.eq(result.cursorsKilled, [cursor.id]);
            const lines = assert.commandWorked(testDB.adminCommand({getLog: "global"})).log;
            const entry = lines
                .map((line) => JSON.parse(line))
                .find(
                    (entry) =>
                        entry.msg === "Slow query" && entry.attr.command?.comment === comment,
                );
            assert.neq(entry, undefined);
            assert.gte(entry.attr.numInterruptChecks, 1, {entry});
            assert.commandWorked(testDB.runCommand({killCursors: "local", cursors: [cursor.id]}));
        });
    }
});
