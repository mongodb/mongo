/**
 * Ensures that the join optimizer chooses a single table access plan for all involved collections
 * based on those collections' indexes and cardinalities. Regression test for SERVER-114340.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {getWinningPlanFromExplain, getAllPlanStages} from "jstests/libs/query/analyze_plan.js";

function joinOptimizerUsed(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    return stages.some((stage) => stage.includes("JOIN_EMBEDDING"));
}

let conn = MongoRunner.runMongod();

const db = conn.getDB("test");

const coll1 = db[jsTestName()];
const coll2 = db[jsTestName() + "_2"];
coll1.drop();
coll2.drop();

let docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i, a: 1, b: i, c: i});
}
assert.commandWorked(coll1.insertMany(docs));
assert.commandWorked(coll1.createIndexes([{a: 1}, {b: 1}]));

for (let i = 10; i < 100; i++) {
    docs.push({_id: i, a: 1, b: i, c: i});
}
assert.commandWorked(coll2.insertMany(docs));

const pipeline = [
    {
        $match: {a: 1, b: 1},
    },
    {
        $lookup: {
            from: coll2.getName(),
            localField: "b",
            foreignField: "b",
            // Note: Single-table predicate on a:1.
            pipeline: [{$match: {a: 1}}],
            as: "coll2",
        },
    },
    {
        $unwind: "$coll2",
    },
];

assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

// Previously this query failed due to using the wrong base collection CE for coll2.
const res = coll1.aggregate(pipeline).toArray();
assert.eq(res.length, 1);
assert.docEq(res[0], {_id: 1, a: 1, b: 1, c: 1, coll2: {_id: 1, a: 1, b: 1, c: 1}});

const explain = coll1.explain().aggregate(pipeline);
assert(joinOptimizerUsed(explain), "Join optimizer was not used as expected: " + tojson(explain));

MongoRunner.stopMongod(conn);
