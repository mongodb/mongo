/**
 * Tests that when the parameter internalQueryPermitMatchSwappingForComplexRenames is set,
 * then match will swap with complex renames.
 */

import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, linebreak, section, subSection} from "jstests/libs/pretty_md.js";

try {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPermitMatchSwappingForComplexRenames: true}));
    const coll = db.complex_match_swap;
    coll.drop();

    section("Inserting docs:");
    const docs = [
        {_id: 1, z: 11, h: {i: 11}, b: {c: 42}},
        {_id: 2, z: 12, h: {i: 12}, b: {}},
        {_id: 3, z: 13, h: {i: 13}, b: {c: null}},
        {_id: 4, z: 14, h: {i: 14}, b: {c: 42, d: "foo"}},
        {_id: 5, z: 15, h: {i: 15}, b: {c: {e: 42, f: "bar"}}},
        {_id: 6, z: 16, h: {i: 16}, b: {c: {e: 42, f: {g: 9}}, d: "foo"}},
    ];
    code(tojson(docs));

    assert.commandWorked(coll.insert(docs));

    function runPipeline(testCaseName, pipeline) {
        section(testCaseName);
        subSection("Pipeline");
        code(tojsononeline(pipeline));

        // Append {$_internalInhibitOptimization: {}} to the front of the pipeline. This prevents
        // pushdown into the find layer, which means that we can just print the pipeline (without
        // $cursor) to the golden file.
        pipeline.unshift({$_internalInhibitOptimization: {}});

        // Print the results of the query to the golden file.
        subSection("Results");
        code(normalizeArray(coll.aggregate(pipeline).toArray()));

        let explain = coll.explain("queryPlanner").aggregate(pipeline);

        // Since we prevented pushdown into the find layer, we expect an array of pipeline stages to
        // be present in the explain output.
        assert(explain.hasOwnProperty("stages"), explain);

        // Drop the first two stages, since we don't need to see the $cursor or
        // $_inhibitOptimization in the golden output.
        let stages = explain.stages;
        assert.gte(stages.length, 3, explain);
        stages = stages.slice(2);
        subSection("Explain");
        code(tojson(stages));
        linebreak();
    }

    let testCaseName = "Basic inclusion projection";
    let pipeline = [{$project: {_id: 1, a: "$b.c", z: 1}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Basic inclusion projection with excluded _id (variation 1)";
    pipeline = [{$project: {_id: 0, a: "$b.c", z: 1}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Basic inclusion projection with excluded _id (variation 2)";
    pipeline = [{$project: {_id: 0, a: "$b.c"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Exclusion projection followed by inclusion projection";
    pipeline = [{$project: {_id: 0, z: 0}}, {$project: {a: "$b.c"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Basic $addFields";
    pipeline = [{$addFields: {a: "$b.c"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Basic $set";
    pipeline = [{$set: {a: "$b.c"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName =
        "Inclusion projection with a match on a subpath of the renamed path (variation 1)";
    pipeline = [{$project: {_id: 1, a: "$b.c", z: 1}}, {$match: {"a.e": {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName =
        "Inclusion projection with a match on a subpath of the renamed path (variation 2)";
    pipeline = [{$project: {_id: 0, a: "$b.c", z: 1}}, {$match: {"a.e": {$gte: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName =
        "Inclusion projection with a match on a subpath of the renamed path (variation 3)";
    pipeline = [{$project: {_id: 0, a: "$b.c"}}, {$match: {"a.e": {$type: "number"}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Exclusion/inclusion projection with a match on a subpath of the renamed path";
    pipeline =
        [{$project: {_id: 0, z: 0}}, {$project: {a: "$b.c"}}, {$match: {"a.e": {$mod: [7, 0]}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "$addFields with a match on a subpath of the renamed path";
    pipeline = [{$addFields: {a: "$b.c"}}, {$match: {"a.e": {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "$set with a match on a subpath of the renamed path";
    pipeline = [{$set: {a: "$b.c"}}, {$match: {"a.e": {$lte: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Chain of complex renames";
    pipeline = [
        {$project: {_id: 0, n: "$b.c"}},
        {$addFields: {q: "$n.f"}},
        {$set: {r: "$q.g"}},
        {$match: {r: {$eq: 9}}},
    ];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Multiple complex renames";
    pipeline =
        [{$project: {n: "$b.c", q: "$h.i"}}, {$match: {$or: [{n: {$gt: 15}}, {q: {$lt: 13}}]}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Multiple complex renames as successive pipeline stages";
    pipeline = [
        {$project: {n: "$b.c", h: 1}},
        {$addFields: {q: "$h.i"}},
        {$project: {h: 0}},
        {$match: {$or: [{n: {$gt: 15}}, {q: {$lt: 13}}]}},
    ];
    runPipeline(testCaseName, pipeline);

    testCaseName = "$match swaps past rename due to group";
    pipeline = [{$group: {_id: {z: "$z"}}}, {$match: {"_id.z": {$lte: 14}}}];
    runPipeline(testCaseName, pipeline);

    // Here is a case that demonstrates one danger of pushing $match past a complex rename. Even
    // when the data doesn't have arrays, the pipeline itself can introduce arrays.
    testCaseName = "$match swaps past rename in the presence of arrays created by the pipeline";
    pipeline = [
        {$lookup: {from: "complex_match_swap", pipeline: [{$group: {_id: "$a", b: {$push: "$b"}}}], as: "arr"}},
        {$project: {c: "$arr.b"}},
        {$match: {c: {$eq: {}}}},
    ];
    runPipeline(testCaseName, pipeline);

    testCaseName = "$match with $exists swaps past rename";
    pipeline = [{$project: {_id: 0, a: "$b.c", z: 1}}, {$match: {a: {$exists: true}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "$match with $expr swaps past rename";
    pipeline = [{$project: {_id: 0, a: "$b.c", z: 1}}, {$match: {$expr: {$eq: ["$a", 42]}}}];
    runPipeline(testCaseName, pipeline);

    // Bug (SERVER-92824): this should be a negative case, but we have not backported the fix to
    // this branch.
    testCaseName = "Dotted path on the left and the right";
    pipeline = [{$project: {_id: 0, "x.y": "$b.c", z: 1}}, {$match: {"x.y": {$lte: 42}}}];
    runPipeline(testCaseName, pipeline);

    //
    // The remaining test cases are negative tests, meaning that we do not expect the $match to be
    // pushed down.
    //
    testCaseName = "Negative case: conditional projection";
    pipeline = [
        {$project: {a: {$cond: {if: {$eq: [null, "$b.c"]}, then: "$$REMOVE", else: "$b.c"}}}},
        {$match: {a: {$eq: 42}}},
    ];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: field path of length 3";
    pipeline = [{$project: {_id: 1, a: "$b.c.e", z: 1}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: field path of length 3 with _id excluded (variation 1)";
    pipeline = [{$project: {_id: 0, a: "$b.c.e", z: 1}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: field path of length 3 with _id excluded (variation 2)";
    pipeline = [{$project: {_id: 0, a: "$b.c.e"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: $addFields with field path of length 3";
    pipeline = [{$addFields: {a: "$b.c.e"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: $set with field path of length 3";
    pipeline = [{$set: {a: "$b.c.e"}}, {$match: {a: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: field path of length 4";
    pipeline = [{$project: {a: "$b.c.f.g", z: 1}}, {$match: {a: {$eq: 9}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: $match cannot be pushed beneath $replaceRoot";
    pipeline = [{$replaceRoot: {newRoot: "$b"}}, {$match: {c: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName = "Negative case: $match cannot be pushed beneath $replaceWith";
    pipeline = [{$replaceWith: "$b"}, {$match: {c: {$eq: 42}}}];
    runPipeline(testCaseName, pipeline);

    testCaseName =
        "Negative case: $match cannot swap past complex rename when matching on subfield of $group key";
    pipeline = [{$group: {_id: {x: "$b.c"}}}, {$match: {"_id.x.e": {$lte: 42}}}];
    runPipeline(testCaseName, pipeline);
} finally {
    // Reset the parameter to its default value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPermitMatchSwappingForComplexRenames: false}));
}
