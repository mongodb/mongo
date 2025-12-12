/**
 * Tests base collection reordering.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {codeOneLine, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {normalizePlan, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

const coll = db[jsTestName() + "_base"];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {base: 22, a: 2, b: 3},
        {base: 22, a: 2, b: -11},
        {base: 28, a: -2, b: -3},
        {base: 33, a: 2, b: 3},
        {base: 3, a: 2, b: 3},
    ]),
);

const a = db[jsTestName() + "_a"];
a.drop();
assert.commandWorked(
    a.insertMany([
        {base: 22, a: 2, b: 3},
        {base: 33, a: -1, b: -1},
        {base: 27, a: -2, b: -11},
        {base: 28, a: -4, b: -1},
    ]),
);

const b = db[jsTestName() + "_b"];
b.drop();
assert.commandWorked(
    b.insertMany([
        {base: 22, a: 2, b: 3},
        {base: 33, a: 2, b: 3},
        {base: 24, a: 3, b: 4},
        {base: 25, a: 1, b: 1},
        {base: 25, a: 3, b: 1},
    ]),
);

const origParams = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalEnableJoinOptimization: 1,
        internalRandomJoinReorderDefaultToHashJoin: 1,
        internalJoinReorderMode: 1,
    }),
);
delete origParams.ok;

function getStageAbbreviation(stageName) {
    switch (stageName) {
        case "HASH_JOIN_EMBEDDING":
            return "HJ";
        case "NESTED_LOOP_JOIN":
            return "NLJ";
        case "MERGE_JOIN":
            return "MJ";
        default:
            return stageName;
    }
}

function formatEmbeddingField(field) {
    if (field && field !== "none") {
        return field;
    }

    return "_";
}

function abbreviate(node) {
    const abbrev = getStageAbbreviation(node.stage);
    if (abbrev == node.stage) {
        return abbrev;
    }
    const l = formatEmbeddingField(node.leftEmbeddingField);
    const r = formatEmbeddingField(node.rightEmbeddingField);
    const children = node.inputStages.map(abbreviate);
    assert.eq(children.length, 2);
    return `(${abbrev} ${l} = ${children[0]}, ${r} = ${children[1]})`;
}

function getJoinOrder(explain) {
    const winningPlan = normalizePlan(getWinningPlanFromExplain(explain), false /*shouldPlatten*/);
    const x = abbreviate(winningPlan);
    return x;
}

function runSingleTest(subtitle, pipeline, seen = undefined) {
    let joinOrder = undefined;
    if (seen) {
        joinOrder = getJoinOrder(coll.explain().aggregate(pipeline));
        if (seen.has(joinOrder)) {
            return undefined;
        }
        seen.add(joinOrder);
    }
    subSection(subtitle);
    if (joinOrder) {
        codeOneLine(joinOrder, true);
    } else {
        outputAggregationPlanAndResults(coll, pipeline, {}, true, false);
    }
    return coll.aggregate(pipeline).toArray();
}

function runRandomReorderTests(pipeline) {
    try {
        const baseRes = runSingleTest("No join opt", pipeline, false);

        const params = {
            internalJoinReorderMode: "random",
            internalEnableJoinOptimization: true,
            internalRandomJoinReorderDefaultToHashJoin: true,
            // TODO SERVER-111798: Adding implicit edges creates cycles in the graph, which are not supported by the random reordering.
            internalMaxNumberNodesConsideredForImplicitEdges: 0,
        };
        assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
        let seed = 0;
        const seen = new Set();
        while (seed < 12) {
            assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: seed}));
            const res = runSingleTest(`Random reordering with seed ${seed}`, pipeline, seen);
            if (res !== undefined) {
                // Skip seed if we've seen this order before.
                assert(_resultSetsEqualUnordered(baseRes, res), `Results differ between no join opt and seed ${seed}`);
            }
            seed++;
        }
        linebreak();
    } finally {
        // Reset flags.
        assert.commandWorked(db.adminCommand({setParameter: 1, ...origParams}));
    }
}

// A - BASE - B
section("3-Node graph, base node fully connected");
runRandomReorderTests([
    {$lookup: {from: a.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: b.getName(), as: "y", localField: "b", foreignField: "b"}},
    {$unwind: "$y"},
    {$project: {_id: 0, "x._id": 0, "y._id": 0}},
]);

// BASE - A - B
section("3-Node graph, base node connected to one node");
runRandomReorderTests([
    {$lookup: {from: a.getName(), as: "x", localField: "a", foreignField: "a"}},
    {$unwind: "$x"},
    {$lookup: {from: b.getName(), as: "y", localField: "x.b", foreignField: "b"}},
    {$unwind: "$y"},
    {$project: {_id: 0, "x._id": 0, "y._id": 0}},
]);

//   BASE
//  /    \
// A ---- B (could be inferred...)
section("3-Node graph + potentially inferred edge");
runRandomReorderTests([
    {$lookup: {from: a.getName(), as: "x", localField: "base", foreignField: "base"}},
    {$unwind: "$x"},
    {$lookup: {from: b.getName(), as: "y", localField: "base", foreignField: "base"}},
    {$unwind: "$y"},
    {$project: {_id: 0, "x._id": 0, "y._id": 0}},
]);

if (!checkSbeFullFeatureFlagEnabled(db)) {
    // TODO SERVER-114457: re-enable for featureFlagSbeFull once $match constants are correctly extracted
    section("4-Node graph + potentially inferred edges & filters");
    runRandomReorderTests([
        {$match: {b: {$eq: 3}}},
        {$lookup: {from: a.getName(), as: "x", localField: "base", foreignField: "base"}},
        {$unwind: "$x"},
        {$lookup: {from: b.getName(), as: "y", localField: "base", foreignField: "base"}},
        {$unwind: "$y"},
        {
            $lookup: {
                from: coll.getName(),
                as: "z",
                localField: "y.base",
                foreignField: "base",
                pipeline: [{$match: {base: {$gt: 3}}}],
            },
        },
        {$unwind: "$z"},
        {$project: {_id: 0, "x._id": 0, "y._id": 0, "z._id": 0}},
    ]);

    section("5-Node graph + filters");
    runRandomReorderTests([
        {$match: {b: {$eq: 3}}},
        {
            $lookup: {
                from: a.getName(),
                as: "aaa",
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {base: {$in: [22, 33]}}}],
            },
        },
        {$unwind: "$aaa"},
        {
            $lookup: {
                from: b.getName(),
                as: "bbb",
                localField: "b",
                foreignField: "b",
                pipeline: [{$match: {base: {$gt: 20}}}],
            },
        },
        {$unwind: "$bbb"},
        {
            $lookup: {
                from: coll.getName(),
                as: "ccc",
                localField: "aaa.base",
                foreignField: "base",
                pipeline: [{$match: {b: {$lt: 0}}}],
            },
        },
        {$unwind: "$ccc"},
        {
            $lookup: {
                from: b.getName(),
                as: "ddd",
                localField: "base",
                foreignField: "base",
                pipeline: [{$match: {b: {$gt: 0}}}],
            },
        },
        {$unwind: "$ddd"},
        {$project: {_id: 0, "aaa._id": 0, "bbb._id": 0, "ccc._id": 0, "ddd._id": 0}},
    ]);
}
