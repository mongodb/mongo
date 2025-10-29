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
 *    query_intensive_pbt,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";
import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

function getNestedDocumentModel() {
    const scalarArb = fc.oneof(
        fc.integer({min: -100, max: 100}),
        fc.boolean(),
        fc.string({maxLength: 10}),
        fc.constant(null),
    );
    const nestedObjectArb = fc.letrec((tie) => ({
        value: fc.oneof(scalarArb, fc.array(tie("value"), {maxLength: 3}), tie("object")),
        object: fc.record({
            a: tie("value"),
            b: tie("value"),
            c: tie("value"),
            d: tie("value"),
        }),
    })).object;
    const arrayOfNestedObjectArb = fc.array(nestedObjectArb, {maxLength: 4});
    return fc.record({
        a: fc.oneof(scalarArb, nestedObjectArb, arrayOfNestedObjectArb),
        b: fc.oneof(scalarArb, nestedObjectArb, arrayOfNestedObjectArb),
        c: fc.oneof(scalarArb, nestedObjectArb, arrayOfNestedObjectArb),
        d: fc.oneof(scalarArb, nestedObjectArb, arrayOfNestedObjectArb),
    });
}

function getSampleNestedDocumentModel() {
    const sampleDocuments = [
        {a: "foo", b: {c: ["value"]}},
        {a: 1, b: 3},
        {a: 1, c: 2},
        {a: 1},
        {a: 2},
        {a: [1, 2], b: [{c: [3, 4]}]},
        {a: [1, 2]},
        {a: [1, [1]]},
        {a: [1, []]},
        {a: [1, [{a: 1}]]},
        {a: [1, {a: 1}, []]},
        {a: [1, {a: 1}]},
        {a: [1, {a: [1]}, [{a: 1}]]},
        {a: [1, {a: [1]}]},
        {a: [1, {a: []}, [1]]},
        {a: [[1], {a: 1}]},
        {a: [[[42]]]},
        {a: [[], {a: 1}]},
        {a: [[{a: 1}], [{a: 1}]]},
        {a: [[{a: 1}], [{b: 1}]]},
        {a: [[{a: 1}]]},
        {a: [[{a: {a: 1}}], [{a: {a: 1}}]]},
        {a: [[{a: {a: 1}}], [{a: {b: 1}}]]},
        {a: [[{a: {a: 1}}], [{b: {a: 1}}]]},
        {a: [[{a: {a: 1}}]]},
        {a: [[{b: 2}]]},
        {a: [[{b: 3}], {b: 4}]},
        {a: []},
        {a: [{a: 1, b: 1, c: 1}]},
        {a: [{a: 1, b: [], c: 1}]},
        {a: [{a: 1, b: []}]},
        {a: [{a: 1}, [1]]},
        {a: [{a: 1}, []]},
        {a: [{a: 1}, [{a: 1}]]},
        {a: [{a: 1}, {a: 1}]},
        {a: [{a: 1}, {b: 1}, []]},
        {a: [{a: [1, 1]}, {a: [1, 1]}]},
        {a: [{a: [1, 1]}]},
        {a: [{a: [1]}, [{a: 1}]]},
        {a: [{a: [1]}, {a: [1]}]},
        {a: [{a: [1]}, {b: [1]}, [1]]},
        {a: [{a: [1]}, {b: [1]}]},
        {a: [{a: [1]}]},
        {a: [{a: [], b: 1, c: 1}]},
        {a: [{a: [], b: 1}]},
        {a: [{a: [], b: [], c: []}]},
        {a: [{a: [], b: []}]},
        {a: [{a: []}, [1]]},
        {a: [{a: []}, {a: [1]}]},
        {a: [{a: []}, {a: []}]},
        {a: [{a: []}, {b: []}, []]},
        {a: [{a: []}, {b: []}]},
        {a: [{a: []}]},
        {a: [{a: {a: 1}}, {a: {a: 1}}]},
        {a: [{a: {a: 1}}, {a: {b: 2}}]},
        {a: [{a: {a: 1}}]},
        {a: [{a: {a: [1]}}]},
        {a: [{a: {a: {a: []}}}]},
        {a: [{a: {b: 1}}]},
        {
            a: [
                {b: 1, c: 2},
                {b: 3, c: 4},
            ],
        },
        {a: [{b: 1}, {b: 2}]},
        {a: [{b: 1}]},
        {a: [{b: 2}]},
        {
            a: [{b: [{c: 1}, {c: 2}, {d: 3}]}, {b: {d: 4, c: 5}}, {b: [{d: 6}, {c: 7}, {d: 8}]}],
        },
        {a: [{b: [{c: 1}, {c: 2}]}]},
        {a: [{b: [{c: 1}]}, {d: [{e: 2}]}]},
        {a: [{b: [{c: 3}]}, {b: [{c: 4}]}]},
        {a: {a: {a: [{a: 1}, []], b: [1, []], c: [[1], {a: 1}]}}},
        {a: {a: {a: [{a: 1}], b: [{a: 1}], c: [{a: 1}]}}},
        {a: {b: 1, c: 3}, d: 5},
        {a: {b: 1}},
        {a: {b: 2}},
        {a: {b: ["foo", 42]}},
        {b: 2, a: 3},
        {b: 4, a: 2},
        {d: 6, a: {c: 4, b: 2}},
    ];
    return fc.constantFrom(...sampleDocuments);
}

function getDocumentModel() {
    return fc.oneof(getNestedDocumentModel(), getSampleNestedDocumentModel());
}

function generateFieldPath() {
    const fields = ["a", "b", "c", "d"];
    const field = fc.sample(fc.constantFrom(...fields), 1)[0];
    const depth = fc.sample(fc.integer({min: 1, max: 4}), 1)[0];
    let path = `$${field}`;
    for (let i = 1; i < depth; i++) {
        const nextField = fc.sample(fc.constantFrom(...fields), 1)[0];
        path += `.${nextField}`;
    }
    return path;
}

function getFieldPathModel() {
    const sampleFieldPaths = [
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
        generateFieldPath(),
        generateFieldPath(),
        generateFieldPath(),
    ];
    jsTest.log({"sampleFieldPaths": sampleFieldPaths});
    return fc.constantFrom(...sampleFieldPaths);
}

function runTestWithParameter(pipeline, useExtract, numExpectedExtractStages) {
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

function run(pipeline, numExpectedExtractStages) {
    jsTest.log({"Pipeline": pipeline});
    const resultsWithExtract = runTestWithParameter(pipeline, true, numExpectedExtractStages);
    const resultsWithoutExtract = runTestWithParameter(pipeline, false, numExpectedExtractStages);
    assert(resultsEq(resultsWithExtract, resultsWithoutExtract));
}

const originalFrameworkControl = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
const originalFeatureFlagExtract = db.adminCommand({getParameter: 1, featureFlagExtractFieldPathsSbeStage: 1});

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

    const n = 10;
    const documents = [
        ...(() => {
            const nestedModel = getDocumentModel();
            const generatedDocs = fc.sample(nestedModel, n).map((doc, index) => Object.assign(doc, {_id: index}));
            // Quadratic.
            return generatedDocs.filter(function (item, pos) {
                return generatedDocs.indexOf(item) == pos;
            });
        })(),
    ];
    jsTest.log({"documents": documents});

    db.c.drop();
    db.c.insertMany(documents);

    const fieldPaths = fc.sample(getFieldPathModel(), n);
    jsTest.log({"fieldPaths": fieldPaths});

    for (let fp0 of fieldPaths) {
        for (let fp1 of fieldPaths) {
            const indexField = fp0.replace("$", "");
            const coveredPlanExpectExtractStage = fp0.includes(".");

            // Test $match then $project with covered plan.
            assert.commandWorked(db.c.createIndex({[indexField]: 1}));
            const coveredIndexPipeline = [{$match: {[indexField]: {$gt: 0}}}, {$project: {x: fp0, _id: 0}}];
            jsTest.log({"coveredIndexPipeline": coveredIndexPipeline});
            run(coveredIndexPipeline, coveredPlanExpectExtractStage ? 1 : 0);

            // Test $match then $project with fetch plan.
            const fetchPlanExpectExtractStage = fp1.includes(".");
            const fetchIndexPipeline = [
                {$match: {[indexField]: {$gt: 0}}},
                {$project: {x: fp1 /*use the other field*/, _id: 0}},
            ];
            jsTest.log({"fetchIndexPipeline": fetchIndexPipeline});
            run(fetchIndexPipeline, fetchPlanExpectExtractStage ? 1 : 0);

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
                run(pipeline, hasDottedPaths ? 1 : 0 /*numExpectedExtractStages*/);
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
                run(pipeline, hasDottedPaths ? numExpectedExtractStages : numExpectedExtractStagesNoDottedPaths);
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
