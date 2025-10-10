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

function runTestWithParameter(documents, pipeline, useExtract) {
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
        assert.eq(extractStages.length, 1, "Should have one extract_field_paths stage");
        assert.eq(extractStages[0]["stage"], "extract_field_paths", "Stage name should match");
    } else {
        assert.eq(extractStages.length, 0, "Should not have extract_field_paths stage");
    }

    const results = db.c.aggregate(pipeline).toArray();
    return results;
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

    const projects = [
        {x: "$a", y: "$a.b"},
        {x: "$a", y: "$a.c"},
        {x: "$a", y: "$b.c"},
        {x: "$a.a", y: "$a.b", z: "$a.c"},
        {x: "$a.a", y: "$a.b"},
        {x: "$a.a"},
        {x: "$a.a.a", y: "$a.a.b"},
        {x: "$a.a.a", y: "$a.b.a"},
        {x: "$a.a.a"},
        {x: "$a.a.a.a"},
        {x: "$a.a.b", y: "$a.a.c"},
        {x: "$a.a.b"},
        {x: "$a.b", y: "$a.c", z: "$d"},
        {x: "$a.b", y: "$a.c"},
        {x: "$a.b"},
        {x: "$a.b.c", y: "$a.b.d"},
        {x: "$a.b.c", y: "$a.d.e"},
        {x: "$a.b.c"},
        {x: "$a.c"},
    ];

    const fieldPaths = [
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
        "$b.c",
    ];

    jsTest.log("Running $projects");
    for (let projIndex = 0; projIndex < projects.length; projIndex++) {
        const project = projects[projIndex];
        const pipeline = [{$project: project}, {$sort: {_id: 1}}];

        const resultsWithExtract = runTestWithParameter(documents, pipeline, true);
        const resultsWithoutExtract = runTestWithParameter(documents, pipeline, false);

        for (let i = 0; i < resultsWithExtract.length; i++) {
            assert.docEq(resultsWithExtract[i], resultsWithoutExtract[i]);
        }
    }

    jsTest.log("Running $groups");
    let seenExtract = false;
    for (let keyPath of fieldPaths) {
        for (let accPath of fieldPaths) {
            const pipeline = {$group: {_id: {path: keyPath}, pathSum: {$sum: accPath}}};
            // TODO SERVER-111637 revisit this try/catch. Some of these plans do not feed a result obj
            // slot into what would be the extract_field_paths stage, so the "uses extract_field_paths
            // stage assertion" can fail. We expect SERVER-111637 will resolve all these cases.
            try {
                const resultsWithExtract = runTestWithParameter(documents, pipeline, true);
                const resultsWithoutExtract = runTestWithParameter(documents, pipeline, false);
                assert(resultsWithExtract.length > 0);
                assert(resultsWithoutExtract.length > 0);
                for (let i = 0; i < resultsWithExtract.length; i++) {
                    assert.docEq(resultsWithExtract[i], resultsWithoutExtract[i]);
                }
                seenExtract = true;
                jsTest.log({"Pipeline used extract": pipeline});
            } catch {
                jsTest.log({"Pipeline did not use extract": pipeline});
            }
        }
    }
    assert.eq(seenExtract, true, "expected at least one $group pipeline to use extract stage");

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
