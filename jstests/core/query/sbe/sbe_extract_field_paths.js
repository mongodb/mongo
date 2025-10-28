/**
 * Tests basic usage of ExtractFieldPathsStage, an SBE stage that extracts multiple paths from an
 * input object in a single pass.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    # Explain for the aggregate command cannot run within a multi-document transaction.
 *    does_not_support_transactions,
 *    not_allowed_with_signed_security_token,
 *    requires_fcv_83,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";
import {resultsEq} from "jstests/aggregation/extras/utils.js";

function runTestWithParameter(documents, pipeline, useExtract, numExpectedExtractStages) {
    db.c.deleteMany({});
    db.c.insertMany(documents);

    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagExtractFieldPathsSbeStage: useExtract,
        }),
    );

    const explain = db.c.explain("executionStats").aggregate(pipeline);
    assert.eq(getEngine(explain), "sbe", "Should use SBE engine");

    // Verify extract_field_paths stage exists
    const extractStages = getSbePlanStages(explain, "extract_field_paths");
    if (useExtract) {
        assert.eq(extractStages.length, numExpectedExtractStages, "Should have extract_field_paths stage(s)");
        for (let extractStage of extractStages) {
            assert.eq(extractStage["stage"], "extract_field_paths", "Stage name should match");
        }
    } else {
        assert.eq(extractStages.length, 0, "Should not have extract_field_paths stage");
    }

    const results = db.c.aggregate(pipeline).toArray();
    return results;
}

function run(documents, pipeline, numExpectedExtractStages) {
    jsTest.log({"Pipeline": pipeline});
    const resultsWithExtract = runTestWithParameter(documents, pipeline, true, numExpectedExtractStages);
    const resultsWithoutExtract = runTestWithParameter(documents, pipeline, false, numExpectedExtractStages);
    assert(resultsEq(resultsWithExtract, resultsWithoutExtract));
}

const originalFrameworkControl = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
const originalFeatureFlagExtract = db.adminCommand({getParameter: 1, featureFlagExtractFieldPathsSbeStage: 1});

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

    const documents = [
        {_id: 0, a: "foo", b: {c: ["value"]}},
        {_id: 1, a: 1, b: 3},
        {_id: 2, a: 1, c: 2},
        {_id: 3, a: 1},
        {_id: 4, a: 2},
        {_id: 5, a: [1, 2], b: [{c: [3, 4]}]},
        {_id: 6, a: [1, 2]},
        {_id: 7, a: [1, [1]]},
        {_id: 8, a: [1, []]},
        {_id: 9, a: [1, [{a: 1}]]},
        {_id: 10, a: [1, {a: 1}, []]},
        {_id: 11, a: [1, {a: 1}]},
        {_id: 12, a: [1, {a: [1]}, [{a: 1}]]},
        {_id: 13, a: [1, {a: [1]}]},
        {_id: 14, a: [1, {a: []}, [1]]},
        {_id: 15, a: [[1], {a: 1}]},
        {_id: 16, a: [[[42]]]},
        {_id: 17, a: [[], {a: 1}]},
        {_id: 18, a: [[{a: 1}], [{a: 1}]]},
        {_id: 19, a: [[{a: 1}], [{b: 1}]]},
        {_id: 20, a: [[{a: 1}]]},
        {_id: 21, a: [[{a: {a: 1}}], [{a: {a: 1}}]]},
        {_id: 22, a: [[{a: {a: 1}}], [{a: {b: 1}}]]},
        {_id: 23, a: [[{a: {a: 1}}], [{b: {a: 1}}]]},
        {_id: 24, a: [[{a: {a: 1}}]]},
        {_id: 25, a: [[{b: 2}]]},
        {_id: 26, a: [[{b: 3}], {b: 4}]},
        {_id: 27, a: []},
        {_id: 28, a: [{a: 1, b: 1, c: 1}]},
        {_id: 29, a: [{a: 1, b: [], c: 1}]},
        {_id: 30, a: [{a: 1, b: []}]},
        {_id: 31, a: [{a: 1}, [1]]},
        {_id: 32, a: [{a: 1}, []]},
        {_id: 33, a: [{a: 1}, [{a: 1}]]},
        {_id: 34, a: [{a: 1}, {a: 1}]},
        {_id: 35, a: [{a: 1}, {b: 1}, []]},
        {_id: 36, a: [{a: [1, 1]}, {a: [1, 1]}]},
        {_id: 37, a: [{a: [1, 1]}]},
        {_id: 38, a: [{a: [1]}, [{a: 1}]]},
        {_id: 39, a: [{a: [1]}, {a: [1]}]},
        {_id: 40, a: [{a: [1]}, {b: [1]}, [1]]},
        {_id: 41, a: [{a: [1]}, {b: [1]}]},
        {_id: 42, a: [{a: [1]}]},
        {_id: 43, a: [{a: [], b: 1, c: 1}]},
        {_id: 44, a: [{a: [], b: 1}]},
        {_id: 45, a: [{a: [], b: [], c: []}]},
        {_id: 46, a: [{a: [], b: []}]},
        {_id: 47, a: [{a: []}, [1]]},
        {_id: 48, a: [{a: []}, {a: [1]}]},
        {_id: 49, a: [{a: []}, {a: []}]},
        {_id: 50, a: [{a: []}, {b: []}, []]},
        {_id: 51, a: [{a: []}, {b: []}]},
        {_id: 52, a: [{a: []}]},
        {_id: 53, a: [{a: {a: 1}}, {a: {a: 1}}]},
        {_id: 54, a: [{a: {a: 1}}, {a: {b: 2}}]},
        {_id: 55, a: [{a: {a: 1}}]},
        {_id: 56, a: [{a: {a: [1]}}]},
        {_id: 57, a: [{a: {a: {a: []}}}]},
        {_id: 58, a: [{a: {b: 1}}]},
        {
            _id: 59,
            a: [
                {b: 1, c: 2},
                {b: 3, c: 4},
            ],
        },
        {_id: 60, a: [{b: 1}, {b: 2}]},
        {_id: 61, a: [{b: 1}]},
        {_id: 62, a: [{b: 2}]},
        {
            _id: 63,
            a: [{b: [{c: 1}, {c: 2}, {d: 3}]}, {b: {d: 4, c: 5}}, {b: [{d: 6}, {c: 7}, {d: 8}]}],
        },
        {_id: 64, a: [{b: [{c: 1}, {c: 2}]}]},
        {_id: 65, a: [{b: [{c: 1}]}, {d: [{e: 2}]}]},
        {_id: 66, a: [{b: [{c: 3}]}, {b: [{c: 4}]}]},
        {_id: 67, a: {a: {a: [{a: 1}, []], b: [1, []], c: [[1], {a: 1}]}}},
        {_id: 68, a: {a: {a: [{a: 1}], b: [{a: 1}], c: [{a: 1}]}}},
        {_id: 69, a: {b: 1, c: 3}, d: 5},
        {_id: 70, a: {b: 1}},
        {_id: 71, a: {b: 2}},
        {_id: 72, a: {b: ["foo", 42]}},
        {_id: 73, b: 2, a: 3},
        {_id: 74, b: 4, a: 2},
        {_id: 75, d: 6, a: {c: 4, b: 2}},
    ];

    const fieldPaths = [
        "$a",
        "$a.a",
        "$a.a.a",
        "$a.a.a.a",
        "$a.a.b",
        "$a.a.c",
        "$a.b",
        "$a.b.a",
        "$a.b.c",
        "$a.b.d",
        "$a.c",
        "$a.d.e",
        "$b",
        "$b.c",
        "$c",
        "$d",
    ];

    for (let fp0 of fieldPaths) {
        for (let fp1 of fieldPaths) {
            const indexField = fp0.replace("$", "");
            const coveredPlanExpectExtractStage = fp0.includes(".");

            // Test $match then $project with covered plan.
            assert.commandWorked(db.c.createIndex({[indexField]: 1}));
            const coveredIndexPipeline = [{$match: {[indexField]: {$gt: 0}}}, {$project: {x: fp0, _id: 0}}];
            jsTest.log({"coveredIndexPipeline": coveredIndexPipeline});
            run(documents, coveredIndexPipeline, coveredPlanExpectExtractStage ? 1 : 0);

            // Test $match then $project with fetch plan.
            const fetchPlanExpectExtractStage = fp1.includes(".");
            const fetchIndexPipeline = [
                {$match: {[indexField]: {$gt: 0}}},
                {$project: {x: fp1 /*use the other field*/, _id: 0}},
            ];
            jsTest.log({"fetchIndexPipeline": fetchIndexPipeline});
            run(documents, fetchIndexPipeline, fetchPlanExpectExtractStage ? 1 : 0);

            assert(db.c.getIndexes().length > 1, "Index should still exist");
            assert.commandWorked(db.c.dropIndex({[indexField]: 1}));
            assert(db.c.getIndexes().length === 1, "Only _id index should still exist");

            // Test $group and $project.
            const hasDottedPaths = fp0.includes(".") || fp1.includes(".");
            const oneExtractStagePipelines = [
                [{$project: {x: fp0, y: fp1}}],
                [{$group: {_id: {path: fp0}, pathSum: {$sum: fp1}}}],
            ];
            for (let pipeline of oneExtractStagePipelines) {
                run(documents, pipeline, hasDottedPaths ? 1 : 0 /*numExpectedExtractStages*/);
            }

            // Test $group then $project and $project then $group.
            const twoExtractStagePipelines = [
                {
                    pipeline: [{$project: {x: fp0, y: fp1}}, {$group: {_id: {path: "$x"}, pathSum: {$sum: "$y"}}}],
                    numExpectedExtractStages: 1,
                    numExpectedExtractStagesNoDottedPaths: 0,
                },
                {
                    pipeline: [
                        {$group: {_id: {path: fp0}, pathSum: {$sum: fp1}}},
                        {$project: {x: "$_id.path", total: "$pathSum"}},
                    ],
                    numExpectedExtractStages: 2,
                    numExpectedExtractStagesNoDottedPaths: 1,
                },
            ];
            jsTest.log({"twoExtractStagePipelines": twoExtractStagePipelines});
            for (let i = 0; i < twoExtractStagePipelines.length; i++) {
                const pipeline = twoExtractStagePipelines[i].pipeline;
                const numExpectedExtractStages = twoExtractStagePipelines[i].numExpectedExtractStages;
                const numExpectedExtractStagesNoDottedPaths =
                    twoExtractStagePipelines[i].numExpectedExtractStagesNoDottedPaths;
                run(
                    documents,
                    pipeline,
                    hasDottedPaths ? numExpectedExtractStages : numExpectedExtractStagesNoDottedPaths,
                );
            }
        }
    }

    jsTest.log("All ExtractFieldPathsStage tests completed successfully!");
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: originalFrameworkControl.internalQueryFrameworkControl,
        }),
    );
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagExtractFieldPathsSbeStage: originalFeatureFlagExtract.featureFlagExtractFieldPathsSbeStage,
        }),
    );
}
