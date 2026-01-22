/**
 * Ensures that the join optimizer populates estimate information (e.g., CE) in the explain output.
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe
 * ]
 */

import {getWinningPlanFromExplain, getAllPlanStages, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

let conn = MongoRunner.runMongod();

const db = conn.getDB("test");

const coll1 = db[jsTestName()];
const coll2 = db[jsTestName() + "_2"];
const coll3 = db[jsTestName() + "_3"];

coll1.drop();
coll2.drop();
coll3.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i, a: i, b: i, c: i, d: i});
}
assert.commandWorked(coll1.insertMany(docs));
assert.commandWorked(coll2.insertMany(docs));
assert.commandWorked(coll3.insertMany(docs));
assert.commandWorked(coll3.createIndex({d: 1}));

// Runs the pipeline, and asserts that the join optimizer was used and that estimate information is
// present in the explain output.
function runTest(pipeline) {
    const explain = coll1.explain().aggregate(pipeline);

    const queryPlanner = getQueryPlanner(explain);
    const usedJoinOptimization = queryPlanner.winningPlan.hasOwnProperty("usedJoinOptimization")
        ? queryPlanner.winningPlan.usedJoinOptimization
        : false;
    assert(usedJoinOptimization, "Join optimizer was not used as expected: " + tojson(explain));

    jsTest.log.info("Explain output: " + tojson(explain));
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain));
    assert(
        stages.some((stage) => stage.stage.includes("JOIN_EMBEDDING")),
        "Expecting JOIN_EMBEDDING stage in: " + tojson(explain),
    );

    for (const stage of stages) {
        // TODO SERVER-111913: Once we have estimates from single table nodes, we can extend this to
        // check other kinds of stages and test that numDocs/keys are reported in the output.
        // TODO SERVER-116505: Verify that cost info is present in explain here too.
        if (stage.stage.includes("JOIN_EMBEDDING") || stage.stage.includes("COLLSCAN")) {
            assert(
                stage.hasOwnProperty("cardinalityEstimate"),
                "Estimates not found in stage: " + tojson(stage) + ", " + tojson(explain),
            );
            assert.gt(stage.cardinalityEstimate, 0, "Cardinality estimate is not greater than 0");
        }
    }
}

assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

// This pipeline has three single-table predicates, where one collection has a supporting index and
// the other two do not.
const pipeline = [
    {$match: {d: {$gte: 0}}},
    {
        $lookup: {
            from: coll2.getName(),
            localField: "b",
            foreignField: "b",
            as: "coll2",
            pipeline: [{$match: {d: {$gte: 0}}}],
        },
    },
    {$unwind: "$coll2"},
    {
        $lookup: {
            from: coll3.getName(),
            localField: "c",
            foreignField: "c",
            as: "coll3",
            pipeline: [{$match: {d: {$gte: 0}}}],
        },
    },
    {$unwind: "$coll3"},
];

// No indexes.
runTest(pipeline);

// With indexes.
assert.commandWorked(coll2.createIndex({b: 1}));
assert.commandWorked(coll3.createIndex({c: 1}));
runTest(pipeline);

MongoRunner.stopMongod(conn);
