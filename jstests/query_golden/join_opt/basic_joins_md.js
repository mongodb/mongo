/**
 * Run basic tests that validate we enter join ordering logic.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {line, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

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

function verifyExplainOutput(pipeline, joinOptExpectedInExplainOutput) {
    const explain = coll.explain().aggregate(pipeline);
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

function runBasicJoinTest(pipeline) {
    try {
        subSection("No join opt");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, false /* noLineBreak*/);
        const noJoinOptResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, false /* joinOptExpectedInExplainOutput */);

        subSection("With bottom-up plan enumeration (left-deep)");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalJoinReorderMode: "bottomUp",
                internalJoinPlanTreeShape: "leftDeep",
            }),
        );
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const bottomUpLeftDeepResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, bottomUpLeftDeepResults),
            "Results differ between no join opt and bottom-up left-deep join enumeration",
        );

        subSection("With bottom-up plan enumeration (right-deep)");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: "rightDeep"}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const bottomUpRightDeepResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, bottomUpRightDeepResults),
            "Results differ between no join opt and bottom-up right-deep join enumeration",
        );

        subSection("With bottom-up plan enumeration (zig-zag)");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: "zigZag"}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const bottomUpZigZagResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, bottomUpZigZagResults),
            "Results differ between no join opt and bottom-up zig-zag join enumeration",
        );

        subSection("With random order, seed 44, nested loop joins");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinReorderMode: "random"}));
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: 44}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const seed44NLJResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, seed44NLJResults),
            "Results differ between no join opt and seed 44 NLJ",
        );

        subSection("With random order, seed 44, hash join enabled");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinReorderDefaultToHashJoin: true}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const seed44HJResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, seed44HJResults),
            "Results differ between no join opt and seed 44 HJ",
        );

        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinReorderDefaultToHashJoin: false}));

        subSection("With random order, seed 420, nested loop joins");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: 420}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const seed420NLJResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, seed420NLJResults),
            "Results differ between no join opt and seed 420 NLJ",
        );

        subSection("With random order, seed 420, hash join enabled");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinReorderDefaultToHashJoin: true}));
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        const seed420HJResults = coll.aggregate(pipeline).toArray();
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, seed420HJResults),
            "Results differ between no join opt and seed 420 HJ",
        );

        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinReorderDefaultToHashJoin: false}));
        foreignColl1.createIndex({a: 1});
        foreignColl2.createIndex({b: 1});
        subSection("With fixed order, index join");

        outputAggregationPlanAndResults(coll, pipeline, {}, true, false, true /* noLineBreak*/);
        verifyExplainOutput(pipeline, true /* joinOptExpectedInExplainOutput */);
        const seedINLJResults = coll.aggregate(pipeline).toArray();
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, seedINLJResults),
            "Results differ between no join opt and INLJ",
        );

        subSection("With bottom-up plan enumeration and indexes");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalJoinReorderMode: "bottomUp",
                internalJoinPlanTreeShape: "leftDeep",
            }),
        );
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false);
        const bottomUpINLJResults = coll.aggregate(pipeline).toArray();
        assert(
            _resultSetsEqualUnordered(noJoinOptResults, bottomUpINLJResults),
            "Results differ between no join opt and INLJ",
        );

        foreignColl1.dropIndex({a: 1});
        foreignColl2.dropIndex({b: 1});
    } finally {
        // Reset flags.
        assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinReorderDefaultToHashJoin: false}));
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinReorderMode: "bottomUp"}));
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: "zigZag"}));
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
