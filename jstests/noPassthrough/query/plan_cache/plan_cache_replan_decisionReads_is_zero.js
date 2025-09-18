/**
 * For the scenario where the multi-planner cached a winning plan that hit EOF immediately (with
 * totalKeysExamined=0 and totalDocsExamined=0) during its trial run, this test verifies that
 * replanning will be triggered if the cached plans starts performing signficantly worse.
 *
 * This test was adapted from a repro for a bug fixed by SERVER-109309.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

// Define two batches of docs that will be inserted into the collection during this test.
let docs1 = [];
let docs2 = [];
for (let i = 0; i < 200; ++i) {
    docs1.push({x: i % 2, y: Math.floor(i / 4)});
}
for (let i = 0; i < 400; ++i) {
    docs2.push({x: (i % 2) + 2, y: Math.floor(i / 8)});
}

// Create an index on "x" and an index on "y".
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

// Enable profiling.
assert.commandWorked(db.setProfilingLevel(2));

// Insert the first batch of documents into the collection.
assert.commandWorked(coll.insert(docs1));

// Define the pipeline that will be used for this test.
//
// It's important that the "x" values in the $match's filter are greater than the "x" values of all
// of the documents that are currently in the collection, so that WiredTigerIndexCursorBase::seek()
// will return boost::none when MongoDB attempts to seek x=2 and x=3 in the {x: 1} index. This
// ensures that "totalKeysExamined" will be 0 (which is important to ensure this test covers the
// code paths relevant to SERVER-109309).
const pipeline =
    [{$match: {x: {$in: [2, 3]}, y: 47}}, {$group: {_id: null, num: {$sum: NumberInt(1)}}}];

// Run the aggregate() command twice so that it's entry in the plan cache gets marked as "active".
for (let i = 0; i < 2; ++i) {
    assert.eq(0, coll.aggregate(pipeline).itcount());
}

// Run explain and get the winning plan from "allPlansExecution".
let explain = coll.explain("allPlansExecution").aggregate(pipeline);
let executionStats = explain.hasOwnProperty("executionStats")
    ? explain.executionStats
    : explain.stages[0].$cursor.executionStats;

let winningPlan = executionStats.allPlansExecution[0];

// The plan using index {x: 1} should be the winning plan here. Because there are currently no
// documents in the collection with x >= 2, this plan will hit EOF immediately (and both calls
// to WiredTigerIndexCursorBase::seek() on index {x: 1} will return boost::none).
//
// Verify that "totalKeysExamined" and "totalDocsExamined" for the winning plan are both zero.
// It's important that we use explain("allPlansExecution") here because we specifically want the
// stats from the trial run (because the trial run's stats are used to compute "decisionReads"),
// and the stats from the profiler ("keysExamined" and "docsExamined") could be slightly different
// (due to differences in SBE vs classic).
assert.eq(0, winningPlan.totalKeysExamined, () => tojson(explain));
assert.eq(0, winningPlan.totalDocsExamined, () => tojson(explain));

// Insert the second batch of documents into the collection.
assert.commandWorked(coll.insert(docs2));

// Run the aggregate() command one more time.
assert.eq(1, coll.aggregate(pipeline).itcount());

// Get the profile entry for the most recent command.
let entry = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});

// Re-planning should have occurred (because the old plan should perform much worse now that we've
// inserted hundreds of documents with x=2 and x=3) and the plan using index {y: 1} should be the
// winning plan now.
assert.eq(entry.replanned, true, () => tojson(entry));
assert.includes(
    entry.replanReason, "cached plan was less efficient than expected", () => tojson(entry));

// Verify that the query examined fewer than 100 keys and 100 docs. In the event that re-planning
// hasn't happened yet, we don't want to force it to happen by running explain("allPlansExecution"),
// so for these checks we use the stats reported by the profiler instead.
assert.lt(entry.keysExamined, 100, () => tojson(entry));
assert.lt(entry.docsExamined, 100, () => tojson(entry));

MongoRunner.stopMongod(conn);
