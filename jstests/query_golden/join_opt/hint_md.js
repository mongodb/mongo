/**
 * Tests hinting joins orders.
 *
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe
 * ]
 */
import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, line, linebreak, subSection} from "jstests/libs/query/pretty_md.js";
import {getJoinOrderOneLine, getWinningJoinOrderOneLine} from "jstests/query_golden/libs/pretty_plan.js";
import {getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {arrayEq, assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {joinTestWrapper, joinOptUsed} from "jstests/libs/query/join_utils.js";

const a = db[jsTestName() + "_a"];
const b = db[jsTestName() + "_b"];
const c = db[jsTestName() + "_c"];

let docs = [];
for (let i = 0; i < 23; i++) {
    docs.push({_id: i, idA: i, idB: i % 12, idC: i % 4});
}
for (const coll of [a, b, c]) {
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndexes([{idA: 1}, {idB: -1}, {idC: 1, idA: -1}]));
}

function getAllOrders(coll, pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    const joinOrder = getWinningJoinOrderOneLine(explain);
    let set = new Set();
    set.add(joinOrder);

    const rejectedPlans = getRejectedPlans(explain);
    for (const plan of rejectedPlans) {
        const order = getJoinOrderOneLine(plan.queryPlan);
        // Orthogonal sanity test: we shouldn't have duplicate orders.
        assert(!set.has(order));
        set.add(order);
    }

    return set;
}

function runTestForHints(coll, pipeline, hintTests) {
    // Get expected results.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalEnableJoinOptimization: false,
        }),
    );
    const expected = coll.aggregate(pipeline).toArray();
    line(`Expected (${expected.length}):`);
    code(normalizeArray(expected));

    // Repeat but get all join orders.
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalEnableJoinOptimization: true,
        }),
    );
    const allOrders = getAllOrders(
        coll,
        [{$_internalJoinHint: {perSubsetLevelMode: [{level: NumberInt(0), mode: "ALL"}]}}].concat(pipeline),
        {perSubsetLevelMode: [{level: NumberInt(0), mode: "ALL"}]},
    );
    subSection(`Total number of join orders: ${allOrders.size}`);

    for (const hintTest of hintTests) {
        const {testCase, hint} = hintTest;
        line(testCase);
        code(tojson(hint));

        // Validate hinted results match!
        const hintedPipeline = [{$_internalJoinHint: hint}].concat(pipeline);
        const actual = coll.aggregate(hintedPipeline).toArray();
        assertArrayEq({expected, actual});

        const hintedOrders = getAllOrders(coll, hintedPipeline, hint);
        hintedOrders.forEach((x) => assert(allOrders.has(x)));
        line(`Num orders: ${hintedOrders.size}`);
        code(Array.from(hintedOrders).sort().join("\n"));
        linebreak();
    }
}

joinTestWrapper(db, () => {
    runTestForHints(
        c,
        [
            {$lookup: {from: a.getName(), as: "foo", localField: "idA", foreignField: "idB"}},
            {$unwind: "$foo"},
            {$lookup: {from: b.getName(), as: "bar", localField: "foo.idB", foreignField: "idC"}},
            {$unwind: "$bar"},
            {$sort: {_id: 1, "foo._id": 1, "bar._id": 1}}, // Ensure we get the same result order.
        ],
        [
            {
                testCase: "INLJ only, all plans",
                hint: {perSubsetLevelMode: [{level: NumberInt(0), hint: {method: "INLJ"}, mode: "ALL"}]},
            },
            {
                testCase: "HJ only, all plans",
                hint: {perSubsetLevelMode: [{level: NumberInt(0), hint: {method: "HJ"}, mode: "ALL"}]},
            },
            {
                testCase: "Fixed methods: HJ then NLJ, all plans",
                hint: {
                    perSubsetLevelMode: [
                        {level: NumberInt(0), mode: "ALL"},
                        {level: NumberInt(1), hint: {method: "HJ"}, mode: "ALL"},
                        {level: NumberInt(2), hint: {method: "NLJ"}, mode: "ALL"},
                    ],
                },
            },
            {
                testCase: "Syntactic join order, left-deep, all plans",
                hint: {
                    planShape: "leftDeep",
                    perSubsetLevelMode: [
                        {level: NumberInt(0), hint: {node: NumberInt(0)}, mode: "ALL"},
                        {level: NumberInt(1), hint: {node: NumberInt(1)}, mode: "ALL"},
                        {level: NumberInt(2), hint: {node: NumberInt(2)}, mode: "ALL"},
                    ],
                },
            },
            {
                testCase: "Syntactic join order, right-deep, all plans",
                hint: {
                    planShape: "rightDeep",
                    perSubsetLevelMode: [
                        {level: NumberInt(0), hint: {node: NumberInt(0)}, mode: "ALL"},
                        {level: NumberInt(1), hint: {node: NumberInt(1)}, mode: "ALL"},
                        {level: NumberInt(2), hint: {node: NumberInt(2)}, mode: "ALL"},
                    ],
                },
            },
            {
                testCase: "Reverse-syntactic order, right-deep using 'isLeftChild', all plans",
                hint: {
                    perSubsetLevelMode: [
                        {level: NumberInt(0), hint: {node: NumberInt(2)}, mode: "ALL"},
                        {level: NumberInt(1), hint: {node: NumberInt(1), isLeftChild: true}, mode: "ALL"},
                        {level: NumberInt(2), hint: {node: NumberInt(0), isLeftChild: true}, mode: "ALL"},
                    ],
                },
            },
            {
                testCase:
                    "Specify another order, right-deep using 'isLeftChild', cheapest plan (note: should appear in previous list!)",
                hint: {
                    perSubsetLevelMode: [
                        {level: NumberInt(0), hint: {node: NumberInt(2)}, mode: "CHEAPEST"},
                        {level: NumberInt(1), hint: {node: NumberInt(1), isLeftChild: true}, mode: "CHEAPEST"},
                        {level: NumberInt(2), hint: {node: NumberInt(0), isLeftChild: true}, mode: "CHEAPEST"},
                    ],
                },
            },
        ],
    );
});
