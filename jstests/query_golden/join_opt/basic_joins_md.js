/**
 * Run basic tests that validate we enter join ordering logic.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {code, section, subSection} from "jstests/libs/query/pretty_md.js";
import {runJoinTestAndCompare} from "jstests/query_golden/libs/join_opt.js";
import {normalizeArray} from "jstests/libs/query_optimization/golden_test.js";
import {joinTestWrapper, joinOptUsed} from "jstests/libs/query/join_utils.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
        {_id: 3, a: null, b: "bar"},
        {_id: 4, b: "bar"},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll.createIndex({dummy: 1, a: -1, b: 1}));

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();

assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
        {_id: 3, a: null, c: "x", d: 4},
        {_id: 4, c: "x", d: 5},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreignColl1.createIndex({dummy: 1, a: -1, b: 1, c: -1, d: 1}));

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", d: 2},
        {_id: 1, b: "bar", d: 6},
        {_id: 2, b: "baz", d: 7},
    ]),
);
// Add index for multikeyness info for path arrayness.
assert.commandWorked(foreignColl2.createIndex({dummy: 1, a: -1, b: 1, d: 1}));

function runBasicJoinTest(pipeline) {
    subSection("No join opt");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

    subSection("Pipeline");
    code(tojson(pipeline));

    // Drop the hint for join opt disabled, we don't support this stage without join opt.
    const hasHint = pipeline[0].$_internalJoinHint;
    const noJoinOptResults = coll.aggregate(hasHint ? pipeline.slice(1) : pipeline).toArray();
    subSection("Results");
    code(normalizeArray(noJoinOptResults));

    const noJoinExplain = coll.explain().aggregate(hasHint ? pipeline.slice(1) : pipeline);
    assert(
        !joinOptUsed(noJoinExplain),
        "Join optimizer was used unexpectedly: " + tojson(noJoinExplain),
    );

    // Enable join opt.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    if (!hasHint) {
        runJoinTestAndCompare(
            "With bottom-up plan enumeration (left-deep)",
            coll,
            pipeline,
            {
                internalJoinReorderMode: "bottomUp",
                internalJoinPlanTreeShape: "leftDeep",
            },
            noJoinOptResults,
        );

        runJoinTestAndCompare(
            "With bottom-up plan enumeration (right-deep)",
            coll,
            pipeline,
            {internalJoinPlanTreeShape: "rightDeep"},
            noJoinOptResults,
        );
    }

    runJoinTestAndCompare(
        "With bottom-up plan enumeration (zig-zag)",
        coll,
        pipeline,
        {internalJoinPlanTreeShape: "zigZag"},
        noJoinOptResults,
    );

    if (!hasHint) {
        for (const internalRandomJoinOrderSeed of [44, 45]) {
            runJoinTestAndCompare(
                `With random order, seed ${internalRandomJoinOrderSeed}`,
                coll,
                pipeline,
                {internalJoinReorderMode: "random", internalRandomJoinOrderSeed},
                noJoinOptResults,
            );
        }

        // Run tests with indexes.
        assert.commandWorked(foreignColl1.createIndex({a: 1}));
        assert.commandWorked(foreignColl2.createIndex({b: 1}));

        runJoinTestAndCompare(
            "With random order, index join",
            coll,
            pipeline,
            {internalJoinReorderMode: "random"},
            noJoinOptResults,
        );

        runJoinTestAndCompare(
            "With bottom-up plan enumeration and indexes",
            coll,
            pipeline,
            {internalJoinReorderMode: "bottomUp", internalJoinPlanTreeShape: "leftDeep"},
            noJoinOptResults,
        );

        assert.commandWorked(foreignColl1.dropIndex({a: 1}));
        assert.commandWorked(foreignColl2.dropIndex({b: 1}));
    }
}

joinTestWrapper(db, () => {
    section("Basic example where $lookup subpipeline contains multiple $match stages");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [
                    {$match: {d: {$lt: 3}}},
                    {$match: {c: "blah"}},
                    {$match: {_id: {$gt: 0}}},
                ],
            },
        },
        {$unwind: "$x"},
    ]);

    section("Basic example with two joins");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
    ]);

    section("Basic example with two joins and suffix");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
        {$sortByCount: "$y.b"},
    ]);

    section("Example with two joins, suffix, and sub-pipeline with un-correlated $match");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$match: {b: {$gt: "aaa"}}}],
            },
        },
        {$unwind: "$y"},
        {$sortByCount: "$x.a"},
    ]);

    section("Example with two joins and sub-pipeline with un-correlated $match");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$match: {b: {$gt: "aaa"}}}],
            },
        },
        {$unwind: "$y"},
    ]);

    section(
        "Example with two joins, suffix, and sub-pipeline with un-correlated $match and $match prefix",
    );
    runBasicJoinTest([
        {$match: {a: {$gt: 1}}},
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$match: {b: {$gt: "aaa"}}}],
            },
        },
        {$unwind: "$y"},
        {$sortByCount: "$x.a"},
    ]);

    section("Example with two joins and sub-pipeline with un-correlated $match and $match prefix");
    runBasicJoinTest([
        {$match: {a: {$gt: 1}}},
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$match: {b: {$gt: "aaa"}}}],
            },
        },
        {$unwind: "$y"},
    ]);

    const foreignColl3 = db[jsTestName() + "_foreign3"];
    foreignColl3.drop();
    assert.commandWorked(
        foreignColl3.insertMany([
            {_id: 0, a: 1, c: "zoo", d: 1},
            {_id: 1, a: 2, c: "blah", d: 2},
            {_id: 2, a: 2, c: "x", d: 3},
        ]),
    );
    // Add index for multikeyness info for path arrayness.
    assert.commandWorked(foreignColl3.createIndex({dummy: -1, a: -1, c: -1, d: -1}));

    section("Basic example with referencing field from previous lookup");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
    ]);

    section(
        "Basic example with 3 joins & subsequent join referencing fields from previous lookups",
    );
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
    ]);

    // TODO: SERVER-113230 Restore this example to use conflicting target paths
    //   {$lookup: {from: foreignColl3.getName(), as: "x.y", localField: "x.c", foreignField: "c"}},
    //   {$unwind: "$x.y"},
    //   {$lookup: {from: foreignColl2.getName(), as: "x.y.z", localField: "x.y.d", foreignField: "d"}},
    //   {$unwind: "$x.y.z"},
    section("Basic example with 3 joins & subsequent join referencing nested paths");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "w.y", localField: "x.c", foreignField: "c"}},
        {$unwind: "$w.y"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "k.y.z",
                localField: "w.y.d",
                foreignField: "d",
            },
        },
        {$unwind: "$k.y.z"},
    ]);

    section("Basic example with a $project excluding a field from the base collection");
    runBasicJoinTest([
        {$project: {_id: false}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
    ]);

    section("Basic example with a $project + rename adding synthetic fields");
    runBasicJoinTest([
        {$project: {a: "my-computed-field", extra: "$a"}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "extra", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
    ]);

    section("Basic example with a $project + rename adding synthetic fields");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                pipeline: [{$project: {a: "my-computed-field", extra: "$a"}}],
            },
        },
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
        {$match: {$expr: {$eq: ["$x.extra", "$a"]}}},
    ]);

    section("Example with a cycle in the join graph");
    runBasicJoinTest([
        {$match: {b: "foo"}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "a", foreignField: "_id"}},
        {$unwind: "$y"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "a", foreignField: "_id"}},
        {$unwind: "$z"},
    ]);

    section("Basic example with $expr predicates");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                let: {a: "$a"},
                pipeline: [{$match: {$expr: {$eq: ["$a", "$$a"]}}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "z",
                let: {b: "$b"},
                pipeline: [{$match: {$expr: {$eq: ["$b", "$$b"]}}}],
            },
        },
        {$unwind: "$z"},
        {$lookup: {from: foreignColl3.getName(), as: "y", localField: "x.c", foreignField: "c"}},
        {$unwind: "$y"},
    ]);

    section(
        "Example with a $lookup with no join predicate but the rest of the pipeline establishes a connected join graph. ",
    );
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "coll12",
                pipeline: [],
            },
        },
        {$unwind: "$coll12"},
        {
            $lookup: {
                from: foreignColl3.getName(),
                let: {a: "$a", a12: "$coll12.a"},
                pipeline: [
                    {$match: {$expr: {$and: [{$eq: ["$a", "$$a"]}, {$eq: ["$a", "$$a12"]}]}}},
                ],
                as: "coll13",
            },
        },
        {$unwind: "$coll13"},
    ]);

    section("Projection on ambiguous field");
    runBasicJoinTest([
        {$lookup: {from: foreignColl2.getName(), as: "x", localField: "a", foreignField: "d"}},
        {$unwind: "$x"},
        {$project: {"_id": 0, "d": 1}},
    ]);

    section("Non-pipeline $lookup with single absorbed $match on as field");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
    ]);

    section("Non-pipeline $lookup with two absorbed $match stages both on as field");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
        {$match: {"x.d": {$eq: 2}}},
    ]);

    section(
        "Non-pipeline $lookup with absorbed $match on as field followed by $match on base field",
    );
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
        {$match: {"b": {$eq: "bar"}}},
    ]);

    section("Two joins where second join has absorbed filter");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
        {$match: {"y.d": {$gt: 2}}},
    ]);

    section(
        "$match referencing the as-field placed before the $lookup that introduces it is a base collection filter and is not absorbed into the joined collection",
    );
    runBasicJoinTest([
        {$match: {"x.c": {$eq: "blah"}}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
    ]);

    section("Pipeline $lookup with pipeline:[] and absorbed $match on as field");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [],
            },
        },
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
    ]);

    section("Pipeline $lookup with pipeline:[$match] and absorbed $match on as field");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}],
            },
        },
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
    ]);

    section("Pipeline $lookup with correlated sub-pipeline and absorbed $match on as field");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                let: {a: "$a"},
                pipeline: [{$match: {$expr: {$eq: ["$a", "$$a"]}}}],
            },
        },
        {$unwind: "$x"},
        {$match: {"x.c": {$eq: "blah"}}},
    ]);

    section("Basic example with a $project including fields from the base collection");
    runBasicJoinTest([
        {$project: {a: 1, b: 1, notUsed: 1}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl2.getName(), as: "y", localField: "b", foreignField: "b"}},
        {$unwind: "$y"},
    ]);

    section(
        "Basic example with a $project including join-predicate fields from foreign collections",
    );
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$project: {a: 1}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$project: {b: 1, c: 1}}],
            },
        },
        {$unwind: "$y"},
    ]);

    section(
        "$project as only stage in subpipeline (no $match), excluding non-join-predicate fields",
    );
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$project: {_id: 0, c: 0}}],
            },
        },
        {$unwind: "$x"},
    ]);

    section("$project in prefix excluding a non-join-predicate field with single join");
    runBasicJoinTest([
        {$project: {_id: 0, b: 0}},
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
            },
        },
        {$unwind: "$x"},
    ]);

    section("Subpipeline with $match followed by multi-field $project excluding non-join fields");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}, {$project: {_id: 0, c: 0}}],
            },
        },
        {$unwind: "$x"},
    ]);

    section(
        "Two joins: first with $match and $project subpipeline, second with $project-only subpipeline",
    );
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}, {$project: {_id: 0}}],
            },
        },
        {$unwind: "$x"},
        {
            $lookup: {
                from: foreignColl2.getName(),
                as: "y",
                localField: "b",
                foreignField: "b",
                pipeline: [{$project: {_id: 0, d: 0}}],
            },
        },
        {$unwind: "$y"},
    ]);

    // Force INLJ with projection on base & subpipeline to test INLJ + proj enumeration & lowering.
    section("Hinted INLJ with a $project");
    assert.commandWorked(coll.createIndex({a: 1}));
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: false},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {a: 1, computed: "foo"}},
        {
            $lookup: {
                from: coll.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$project: {a: 1, computed: "bar"}}],
            },
        },
        {$unwind: "$x"},
        {$project: {_id: 0, "x._id": 0}},
    ]);

    section("Hinted INLJ with a $project, reverse order");
    assert.commandWorked(coll.createIndex({a: 1}));
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: true},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {a: 1, computed: "y"}},
        {
            $lookup: {
                from: coll.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$project: {a: 1, computed: "x"}}],
            },
        },
        {$unwind: "$x"},
        {$project: {_id: 0, "x._id": 0}},
    ]);

    // Now with renames.
    section("Hinted INLJ with a $project + rename on predicate");
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: false},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {m: "$a"}},
        {
            $lookup: {
                from: coll.getName(),
                as: "x",
                localField: "m",
                foreignField: "a",
                pipeline: [{$project: {n: "$a"}}],
            },
        },
        {$unwind: "$x"},
        {$project: {_id: 0, "x._id": 0}},
    ]);

    section("Hinted INLJ with a $project + rename on predicate, reverse order");
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: true},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {m: "$a"}},
        {
            $lookup: {
                from: coll.getName(),
                as: "x",
                localField: "m",
                foreignField: "a",
                pipeline: [{$project: {n: "$a"}}],
            },
        },
        {$unwind: "$x"},
        {$project: {_id: 0, "x._id": 0}},
    ]);

    section("Hinted INLJ with a $project + rename + trailing $match");
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: false},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {m: "$a"}},
        {$lookup: {from: coll.getName(), as: "x", pipeline: [{$project: {n: "$a"}}]}},
        {$unwind: "$x"},
        {$match: {$expr: {$eq: ["$m", "$x.n"]}}},
        {$project: {_id: 0, "x._id": 0}},
    ]);

    section("Hinted INLJ with a $project + rename + trailing $match, reverse order");
    runBasicJoinTest([
        {
            $_internalJoinHint: {
                perSubsetLevelMode: [
                    {level: NumberInt(0), mode: "CHEAPEST"},
                    {
                        level: NumberInt(1),
                        hint: {node: NumberInt(1), method: "INLJ", isLeftChild: true},
                        mode: "CHEAPEST",
                    },
                ],
            },
        },
        {$project: {m: "$a"}},
        {$lookup: {from: coll.getName(), as: "x", pipeline: [{$project: {n: "$a"}}]}},
        {$unwind: "$x"},
        {$match: {$expr: {$eq: ["$m", "$x.n"]}}},
        {$project: {_id: 0, "x._id": 0}},
    ]);
}); // joinTestWrapper();
