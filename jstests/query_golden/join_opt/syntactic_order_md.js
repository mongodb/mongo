/**
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */
import {code, section, subSection} from "jstests/libs/query/pretty_md.js";
import {runJoinTestAndCompare} from "jstests/query_golden/libs/join_opt.js";
import {getWinningJoinOrderOneLine} from "jstests/query_golden/libs/pretty_plan.js";
import {normalizeArray} from "jstests/libs/query_optimization/golden_test.js";
import {joinTestWrapper, joinOptUsed} from "jstests/libs/query/join_utils.js";

// Initialize a one-row collection.
const tiny = db[jsTestName() + "_tiny"];
tiny.drop();
assert.commandWorked(tiny.insertMany([{_id: 0, i_idx: 1}]));
assert.commandWorked(tiny.createIndex({i_idx: 1}));

// Now two larger collections.
const grande = db[jsTestName() + "_grande"];
grande.drop();
const docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({_id: i, i_idx: i});
}
assert.commandWorked(grande.insertMany(docs));
assert.commandWorked(grande.createIndex({i_idx: 1}));

const venti = db[jsTestName() + "_many_rows"];
venti.drop();
// Add one more doc to break ties, otherwise syntactic edge order may change the plan.
docs.push({_id: 10000, i_idx: 10000});
assert.commandWorked(venti.insertMany(docs));
assert.commandWorked(venti.createIndex({i_idx: 1}));

function runJoinTest(test, baseColl, pipeline) {
    section(test);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

    subSection("Pipeline");
    code(tojson(pipeline));

    const noJoinOptResults = baseColl.aggregate(pipeline).toArray();
    subSection("Results");
    code(normalizeArray(noJoinOptResults));

    const noJoinExplain = baseColl.explain().aggregate(pipeline);
    assert(
        !joinOptUsed(noJoinExplain),
        "Join optimizer was used unexpectedly: " + tojson(noJoinExplain),
    );

    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    const joinOrder = getWinningJoinOrderOneLine(baseColl.explain().aggregate(pipeline));
    subSection("Join order");
    code(joinOrder);

    runJoinTestAndCompare(
        `${test} + Join Optimization`,
        baseColl,
        pipeline,
        {internalEnableJoinOptimization: true},
        noJoinOptResults,
    );

    return joinOrder;
}

joinTestWrapper(db, () => {
    const order1 = runJoinTest("Syntactic version 1", tiny, [
        {$lookup: {from: grande.getName(), localField: "i_idx", foreignField: "i_idx", as: "a"}},
        {$unwind: "$a"},
        {$lookup: {from: venti.getName(), localField: "i_idx", foreignField: "i_idx", as: "b"}},
        {$unwind: "$b"},
    ]);
    const order2 = runJoinTest("Syntactic version 2", venti, [
        {$lookup: {from: tiny.getName(), localField: "i_idx", foreignField: "i_idx", as: "b"}},
        {$unwind: "$b"},
        {$lookup: {from: grande.getName(), localField: "i_idx", foreignField: "i_idx", as: "a"}},
        {$unwind: "$a"},
    ]);

    // Embeddings differ- strip the embedding names "a", "b", and and "_".
    // We expect that equivalent join graphs (minus embeddings) will pick the same plan.
    assert.eq(
        order1.replace(/[ab_] = /g, "<> = "),
        order2.replace(/[ab_] = /g, "<> = "),
        "Join orders are not equivalent",
        {order1, order2},
    );
});
