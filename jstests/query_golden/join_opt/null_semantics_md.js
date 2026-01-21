/**
 * Run basic tests that validate join reordering preserves null semantics.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, section, subSection} from "jstests/libs/query/pretty_md.js";
import {verifyExplainOutput, runJoinTestAndCompare, joinTestWrapper} from "jstests/query_golden/libs/join_opt.js";

const testDocs = [
    {_id: 0},
    {_id: 1, key: 1},
    {_id: 2, key: null},
    {_id: 3, key: {}},
    {_id: 4, key: {foo: null}},
    {_id: 5, key: {foo: {}}},
];

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany(testDocs));

const otherColl = db[jsTestName() + "_other"];
otherColl.drop();
assert.commandWorked(otherColl.insertMany(testDocs));

const thirdColl = db[jsTestName() + "_third"];
thirdColl.drop();
assert.commandWorked(thirdColl.insertMany(testDocs));

const testCases = [
    {
        internalJoinReorderMode: "bottomUp",
        internalJoinPlanTreeShape: "zigZag",
    },
    {
        internalJoinReorderMode: "random",
        internalRandomJoinReorderDefaultToHashJoin: true,
        internalRandomJoinOrderSeed: 42,
    },
    {
        internalJoinReorderMode: "random",
        internalRandomJoinReorderDefaultToHashJoin: false,
        internalRandomJoinOrderSeed: 64,
    },
];

function runNullSemanticsTest(pipeline, assertResultsEqual, extraParams = {}) {
    subSection("No join opt");
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

    subSection("Expected results");
    const noJoinOptResults = coll.aggregate(pipeline).toArray();
    code(normalizeArray(noJoinOptResults, true /* shouldSortArray */));

    const noJoinExplain = coll.explain().aggregate(pipeline);
    verifyExplainOutput(noJoinExplain, false /* joinOptExpectedInExplainOutput */);

    // Enable join opt & increase the max number of nodes for join edges.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

    // Run tests.
    for (const params of testCases) {
        let caseName = "";
        for (const [k, v] of Object.entries(params)) {
            if (caseName.length > 0) {
                caseName += ", ";
            }
            caseName += `${k} = ${v}`;
        }
        assert.commandWorked(db.adminCommand({setParameter: 1, ...params, ...extraParams}));
        runJoinTestAndCompare(caseName, coll, pipeline, params, noJoinOptResults, assertResultsEqual);
    }
}

joinTestWrapper(() => {
    section("Simple local-foreign field join");
    runNullSemanticsTest([
        {
            $lookup: {
                from: otherColl.getName(),
                localField: "key",
                foreignField: "key",
                as: "lf",
            },
        },
        {$unwind: "$lf"},
    ]);

    section("Simple local-foreign field join (nested field)");
    runNullSemanticsTest([
        {
            $lookup: {
                from: otherColl.getName(),
                localField: "key.foo",
                foreignField: "key.foo",
                as: "lf",
            },
        },
        {$unwind: "$lf"},
    ]);

    // TODO SERVER-113276: fix.
    section("Correlated sub-pipeline");
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: otherColl.getName(),
                    let: {k: "$key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "cor",
                },
            },
            {$unwind: "$cor"},
        ],
        false /* assertResultsEqual */,
    );

    // TODO SERVER-113276: fix.
    section("Correlated sub-pipeline (nested field)");
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: otherColl.getName(),
                    let: {f: "$key.foo"},
                    pipeline: [{$match: {$expr: {$eq: ["$$f", "$key.foo"]}}}],
                    as: "cor",
                },
            },
            {$unwind: "$cor"},
        ],
        false /* assertResultsEqual */,
    );

    section("Implicit cycle (local-foreign)");
    runNullSemanticsTest([
        {
            $lookup: {
                from: otherColl.getName(),
                localField: "key",
                foreignField: "key",
                as: "lf",
            },
        },
        {$unwind: "$lf"},
        {
            $lookup: {
                from: thirdColl.getName(),
                localField: "lf.key",
                foreignField: "key",
                as: "lf2",
            },
        },
        {$unwind: "$lf2"},
    ]);

    // TODO SERVER-113276: fix.
    section("Implicit cycle (mixed)");
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: otherColl.getName(),
                    localField: "key",
                    foreignField: "key",
                    as: "lf",
                },
            },
            {$unwind: "$lf"},
            {
                $lookup: {
                    from: thirdColl.getName(),
                    let: {f: "$lf.key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$f", "$key.foo"]}}}],
                    as: "cor",
                },
            },
            {$unwind: "$cor"},
        ],
        false /* assertResultsEqual */,
    );

    // TODO SERVER-113276: fix.
    section("Implicit cycle (correlated)");
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: thirdColl.getName(),
                    let: {k: "$key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "cor",
                },
            },
            {$unwind: "$cor"},
            {
                $lookup: {
                    from: otherColl.getName(),
                    let: {f: "$cor.foo"},
                    pipeline: [{$match: {$expr: {$eq: ["$$f", "$key.foo"]}}}],
                    as: "cor2",
                },
            },
            {$unwind: "$cor2"},
        ],
        false /* assertResultsEqual */,
    );

    // Add some indexes.
    assert.commandWorked(otherColl.createIndex({key: 1}));
    assert.commandWorked(otherColl.createIndex({"key.foo": 1}));
    assert.commandWorked(thirdColl.createIndex({key: 1}));

    // TODO SERVER-113276: fix.
    section("Implicit cycle (mixed) + indexes");
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: otherColl.getName(),
                    localField: "key",
                    foreignField: "key",
                    as: "lf",
                },
            },
            {$unwind: "$lf"},
            {
                $lookup: {
                    from: thirdColl.getName(),
                    let: {f: "$lf.key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$f", "$key.foo"]}}}],
                    as: "cor",
                },
            },
            {$unwind: "$cor"},
        ],
        false /* assertResultsEqual */,
    );

    const fourthColl = db[jsTestName() + "_fourth"];
    fourthColl.drop();
    assert.commandWorked(fourthColl.insertMany(testDocs));

    const fifthColl = db[jsTestName() + "_fifth"];
    fifthColl.drop();
    assert.commandWorked(fifthColl.insertMany(testDocs));

    // TODO SERVER-113276: fix.
    section("Large implicit cycle (5 nodes)");
    /**
     * Graph: BASE <-> OTHER <-> THIRD <-> FOURTH <-> FIFTH
     * This lets us create implicit $eq edges between every node to get a fully connected graph.
     * However, the existing edges are $expr edges.
     */
    runNullSemanticsTest(
        [
            {
                $lookup: {
                    from: otherColl.getName(),
                    let: {k: "$key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "c1",
                },
            },
            {$unwind: "$c1"},
            {
                $lookup: {
                    from: thirdColl.getName(),
                    let: {k: "$c1.key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "c2",
                },
            },
            {$unwind: "$c2"},
            {
                $lookup: {
                    from: fourthColl.getName(),
                    let: {k: "$c2.key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "c3",
                },
            },
            {$unwind: "$c3"},
            {
                $lookup: {
                    from: fifthColl.getName(),
                    let: {k: "$c3.key"},
                    pipeline: [{$match: {$expr: {$eq: ["$$k", "$key"]}}}],
                    as: "c4",
                },
            },
            {$unwind: "$c4"},
        ],
        false /* assertResultsEqual */,
        {internalMaxNumberNodesConsideredForImplicitEdges: 5},
    );
}); // joinTestWrapper();
