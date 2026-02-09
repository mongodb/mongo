/**
 * Run basic tests that validate we enter join ordering logic.
 *
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe
 * ]
 */
import {section, subSection} from "jstests/libs/query/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {runJoinTestAndCompare, joinTestWrapper} from "jstests/query_golden/libs/join_opt.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

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

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", d: 2},
        {_id: 1, b: "bar", d: 6},
        {_id: 2, b: "baz", d: 7},
    ]),
);

function runBasicJoinTest(pipeline) {
    subSection("No join opt");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    outputAggregationPlanAndResults(coll, pipeline, {}, true, false, false /* noLineBreak*/);
    const noJoinExplain = coll.explain().aggregate(pipeline);
    const noJoinOptResults = coll.aggregate(pipeline).toArray();
    assert(!joinOptUsed(noJoinExplain), "Join optimizer was not used as expected: " + tojson(noJoinExplain));

    runJoinTestAndCompare(
        "With bottom-up plan enumeration (left-deep)",
        coll,
        pipeline,
        {
            internalEnableJoinOptimization: true,
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

    runJoinTestAndCompare(
        "With bottom-up plan enumeration (zig-zag)",
        coll,
        pipeline,
        {internalJoinPlanTreeShape: "zigZag"},
        noJoinOptResults,
    );

    for (const internalRandomJoinOrderSeed of [44, 45]) {
        runJoinTestAndCompare(
            `With random order, seed ${internalRandomJoinOrderSeed}, nested loop joins`,
            coll,
            pipeline,
            {internalJoinReorderMode: "random", internalRandomJoinOrderSeed},
            noJoinOptResults,
        );

        runJoinTestAndCompare(
            `With random order, seed ${internalRandomJoinOrderSeed}, hash join enabled`,
            coll,
            pipeline,
            {internalRandomJoinReorderDefaultToHashJoin: true},
            noJoinOptResults,
        );
    }

    // Run tests with indexes.
    assert.commandWorked(foreignColl1.createIndex({a: 1}));
    assert.commandWorked(foreignColl2.createIndex({b: 1}));

    runJoinTestAndCompare(
        "With fixed order, index join",
        coll,
        pipeline,
        {internalRandomJoinReorderDefaultToHashJoin: false},
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

joinTestWrapper(() => {
    section("Basic example where $lookup subpipeline contains multiple $match stages");
    runBasicJoinTest([
        {
            $lookup: {
                from: foreignColl1.getName(),
                as: "x",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {d: {$lt: 3}}}, {$match: {c: "blah"}}, {$match: {_id: {$gt: 0}}}],
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

    section("Example with two joins, suffix, and sub-pipeline with un-correlated $match and $match prefix");
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

    section("Basic example with referencing field from previous lookup");
    runBasicJoinTest([
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
    ]);

    section("Basic example with 3 joins & subsequent join referencing fields from previous lookups");
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
        {$lookup: {from: foreignColl2.getName(), as: "k.y.z", localField: "w.y.d", foreignField: "d"}},
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

    section("Basic example with a $project reducing the documents of the base collection to a single field");
    runBasicJoinTest([
        {$project: {a: true}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "a", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
    ]);

    section("Basic example with a $project adding synthetic fields");
    runBasicJoinTest([
        {$project: {a: true, extra: "$a"}},
        {$lookup: {from: foreignColl1.getName(), as: "x", localField: "extra", foreignField: "a"}},
        {$unwind: "$x"},
        {$lookup: {from: foreignColl3.getName(), as: "z", localField: "x.c", foreignField: "c"}},
        {$unwind: "$z"},
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
}); // joinTestWrapper();
