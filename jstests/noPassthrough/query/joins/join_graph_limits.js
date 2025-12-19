/**
 * Validate join graph limits.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getPlanStages} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(`${jsTestName()}_db`);

const docs = [
    {a: 1, b: 1},
    {a: 1, b: 2},
];

function setServerParameters(knobs) {
    assert.commandWorked(db.adminCommand({setParameter: 1, ...knobs}));
}

function assertNonOptimizedLookups(coll, pipeline, expectedLookupStages) {
    const explain = coll.explain().aggregate(pipeline);
    const lookupStages = getPlanStages(explain, "$lookup");
    assert.eq(lookupStages.length, expectedLookupStages);
}

function assertJoinPlanResults(coll, pipeline, expectedResults) {
    const results = coll.aggregate(pipeline).toArray();
    assertArrayEq({expected: expectedResults, actual: results});
}

const numberOfNodes = 5;
const numberOfEdges = 30;
const numberOfJoins = numberOfNodes - 1;

const collName = jsTestName();
const coll = db[collName];

coll.drop();
assert.commandWorked(coll.insertMany(docs));

const pipeline = [];
let prevCollName = null;
for (let i = 0; i < numberOfJoins; ++i) {
    const from = `${collName}${i}`;
    const coll = db[from];
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));

    const localField = prevCollName == null ? "a" : `${prevCollName}.a`;
    const foreignField = "b";

    pipeline.push({"$lookup": {from, localField, foreignField, as: from}});
    pipeline.push({"$unwind": {path: `$${from}`}});

    prevCollName = from;
}

// 1. Join optimization disabled.
setServerParameters({internalEnableJoinOptimization: false});
const noJoinOptResults = coll.aggregate(pipeline).toArray();
// Validate that we haven't got empty results.
assert.eq(noJoinOptResults.length, 2);

setServerParameters({
    internalEnableJoinOptimization: true,
    internalJoinReorderMode: "random",
    internalRandomJoinOrderSeed: 42,
});

// 2. Join optimization enabled: no graph limits hit.
{
    setServerParameters({
        internalMaxNodesInJoinGraph: numberOfNodes,
        internalMaxEdgesInJoinGraph: numberOfEdges,
    });

    // Make sure that the results are still correct.
    assertJoinPlanResults(coll, pipeline, noJoinOptResults);

    // All $lookup stages are expected to be optimized.
    assertNonOptimizedLookups(coll, pipeline, 0);
}

// 3. Join optimization enabled: number of nodes limit hit.
{
    setServerParameters({
        internalMaxNodesInJoinGraph: numberOfJoins,
        internalMaxEdgesInJoinGraph: numberOfEdges,
    });

    // Make sure that the results are still correct.
    assertJoinPlanResults(coll, pipeline, noJoinOptResults);

    // Due to the limit one $lookup stages is expected to be left unoptimized.
    assertNonOptimizedLookups(coll, pipeline, 1);
}

// 4. Join optimization enabled: number of edges limit hit.
{
    setServerParameters({
        internalMaxNodesInJoinGraph: numberOfNodes,
        internalMaxEdgesInJoinGraph: numberOfJoins - 1,
    });

    // Make sure that the results are still correct.
    assertJoinPlanResults(coll, pipeline, noJoinOptResults);

    // Due to the limit one $lookup stages is expected to be left unoptimized.
    assertNonOptimizedLookups(coll, pipeline, 1);
}

MongoRunner.stopMongod(conn);
