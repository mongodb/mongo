/**
 * Run basic tests that validate we enter join ordering logic.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {line, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";
import {prettyPrintWinningPlan} from "jstests/query_golden/libs/pretty_plan.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
    ]),
);

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();
assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
    ]),
);

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", d: 2},
        {_id: 1, b: "bar", d: 6},
    ]),
);

function verifyExplainOutput(explain, joinOptExpectedInExplainOutput) {
    const winningPlan = getQueryPlanner(explain).winningPlan;

    if (joinOptExpectedInExplainOutput) {
        assert(winningPlan.hasOwnProperty("usedJoinOptimization") && winningPlan.usedJoinOptimization, winningPlan);
        // Golden tests utils don't output winningPlan stats so manually record it in this helper function.
        line(`usedJoinOptimization: ${winningPlan.usedJoinOptimization}`);
        linebreak();
        return;
    }

    // If the knob is not enabled, the explain should not include the join optimization flag.
    assert(!("usedJoinOptimization" in winningPlan), winningPlan);
}

function getJoinTestResultsAndExplain(desc, pipeline, params) {
    subSection(desc);
    assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    return [coll.aggregate(pipeline).toArray(), coll.explain().aggregate(pipeline)];
}

function runJoinTestAndCompare(desc, pipeline, params, expected) {
    const [actual, explain] = getJoinTestResultsAndExplain(desc, pipeline, params);
    assertArrayEq({expected, actual});
    verifyExplainOutput(explain, true /* joinOptExpectedInExplainOutput */);
    prettyPrintWinningPlan(explain);
}

function runBasicJoinTest(pipeline) {
    try {
        subSection("No join opt");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, false /* noLineBreak*/);
        const noJoinExplain = coll.explain().aggregate(pipeline);
        const noJoinOptResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(noJoinExplain, false /* joinOptExpectedInExplainOutput */);

        runJoinTestAndCompare(
            "With bottom-up plan enumeration (left-deep)",
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
            pipeline,
            {internalJoinPlanTreeShape: "rightDeep"},
            noJoinOptResults,
        );

        runJoinTestAndCompare(
            "With bottom-up plan enumeration (zig-zag)",
            pipeline,
            {internalJoinPlanTreeShape: "zigZag"},
            noJoinOptResults,
        );

        for (const internalRandomJoinOrderSeed of [44, 45]) {
            runJoinTestAndCompare(
                `With random order, seed ${internalRandomJoinOrderSeed}, nested loop joins`,
                pipeline,
                {internalJoinReorderMode: "random", internalRandomJoinOrderSeed},
                noJoinOptResults,
            );

            runJoinTestAndCompare(
                `With random order, seed ${internalRandomJoinOrderSeed}, hash join enabled`,
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
            pipeline,
            {internalRandomJoinReorderDefaultToHashJoin: false},
            noJoinOptResults,
        );

        runJoinTestAndCompare(
            "With bottom-up plan enumeration and indexes",
            pipeline,
            {internalJoinReorderMode: "bottomUp", internalJoinPlanTreeShape: "leftDeep"},
            noJoinOptResults,
        );

        assert.commandWorked(foreignColl1.dropIndex({a: 1}));
        assert.commandWorked(foreignColl2.dropIndex({b: 1}));
    } finally {
        // Reset flags.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalEnableJoinOptimization: false,
                internalRandomJoinReorderDefaultToHashJoin: false,
                internalJoinReorderMode: "bottomUp",
                internalJoinPlanTreeShape: "zigZag",
            }),
        );
    }
}

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

if (!checkSbeFullFeatureFlagEnabled(db)) {
    // TODO SERVER-114457: re-enable for featureFlagSbeFull once $match constants are correctly extracted
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
}

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
