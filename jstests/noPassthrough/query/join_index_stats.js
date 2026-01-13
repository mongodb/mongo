/**
 * Ensures that the join optimizer correctly reports index stats for the single table access plans
 * for all involved collections. Regression test for SERVER-114567.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {getWinningPlanFromExplain, getAllPlanStages} from "jstests/libs/query/analyze_plan.js";

function joinOptimizerUsed(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    return stages.some((stage) => stage.includes("JOIN_EMBEDDING"));
}

function getUsageCount(indexName, collection) {
    let res = collection.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();
    if (res.length === 0) {
        return undefined;
    }
    return res[0].accesses.ops;
}

let conn = MongoRunner.runMongod();

const db = conn.getDB("test");

const coll1 = db[jsTestName()];
const coll2 = db[jsTestName() + "_2"];
const coll3 = db[jsTestName() + "_3"];

function dropAndRecreateColls() {
    coll1.drop();
    coll2.drop();
    coll3.drop();

    let docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({_id: i, a: i, b: i, c: i});
    }
    assert.commandWorked(coll1.insertMany(docs));
    assert.commandWorked(coll2.insertMany(docs));
    assert.commandWorked(coll3.insertMany(docs));

    assert.commandWorked(coll1.createIndex({a: 1}));
    assert.commandWorked(coll2.createIndex({b: 1}));
}

const pipeline = [
    {
        $match: {a: 1},
    },
    {
        $lookup: {
            from: coll2.getName(),
            localField: "c",
            foreignField: "c",
            // Note: Single-table predicate on b:1.
            pipeline: [{$match: {b: 1}}],
            as: "coll2",
        },
    },
    {
        $unwind: "$coll2",
    },
    {
        $lookup: {
            from: coll3.getName(),
            localField: "c",
            foreignField: "c",
            // Note: Single-table predicate on c:1.
            pipeline: [{$match: {c: 1}}],
            as: "coll3",
        },
    },
    {
        $unwind: "$coll3",
    },
];

assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
dropAndRecreateColls();
const explain = coll1.explain().aggregate(pipeline);
assert(joinOptimizerUsed(explain), "Join optimizer was not used as expected: " + tojson(explain));

//
// Test with random order.
//
assert.commandWorked(conn.adminCommand({setParameter: 1, internalJoinReorderMode: "random"}));

// Try a bunch of different join orders.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: i}));
    coll1.aggregate(pipeline).toArray();
}

// No matter the join order, we should use index scan on "a" for coll1, index scan on "b" for coll2,
// and collection scan for coll3.
assert.eq(getUsageCount("a_1", coll1), 10);
assert.eq(getUsageCount("b_1", coll2), 10);

// We can't grab collection scan count on coll3 directly, so we check the serverStatus metrics instead.
assert.eq(db.runCommand({serverStatus: 1}).metrics.queryExecutor.collectionScans.total, 10);

//
// Test with bottom up order.
//
dropAndRecreateColls();
assert.commandWorked(conn.adminCommand({setParameter: 1, internalJoinReorderMode: "bottomUp"}));

for (let shape of ["leftDeep", "rightDeep", "zigZag"]) {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinPlanTreeShape: shape}));
    coll1.aggregate(pipeline).toArray();
}
// No matter the join order, we should use index scan on "a" for coll1, index scan on "b" for coll2,
// and collection scan for coll3.
assert.eq(getUsageCount("a_1", coll1), 3);
assert.eq(getUsageCount("b_1", coll2), 3);
assert.eq(db.runCommand({serverStatus: 1}).metrics.queryExecutor.collectionScans.total, 13); // 10 from before + 3 now

//
// Test self-lookup.
//
const selfLookupPipe = [
    {
        $match: {a: 1},
    },
    {
        $lookup: {
            from: coll1.getName(),
            localField: "c",
            foreignField: "c",
            // Note: Single-table predicate on b:1.
            pipeline: [{$match: {b: 1}}],
            as: "coll1",
        },
    },
    {
        $unwind: "$coll1",
    },
];
dropAndRecreateColls();
coll1.aggregate(selfLookupPipe).toArray();
const selfLookupExplain = coll1.explain().aggregate(pipeline);
assert(joinOptimizerUsed(selfLookupExplain), "Join optimizer was not used as expected: " + tojson(selfLookupExplain));

assert.eq(getUsageCount("a_1", coll1), 1);
assert.eq(db.runCommand({serverStatus: 1}).metrics.queryExecutor.collectionScans.total, 14); // 13 from before + 1 now

MongoRunner.stopMongod(conn);
