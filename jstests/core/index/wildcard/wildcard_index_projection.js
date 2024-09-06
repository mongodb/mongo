/**
 * Tests that a wildcard index with an exclusion projection but including _id field gets saved
 * properly. Exercises the fix for SERVER-52814.
 * @tags: [
 *   assumes_read_concern_local,
 *   does_not_support_stepdowns,
 *   no_selinux,
 * ]
 */

import {getQueryPlanner, getRejectedPlan, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();
coll.createIndex({"$**": 1}, {wildcardProjection: {name: 0, type: 0, _id: 1}});

const sharded = FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint;

const indexes = coll.getIndexes().filter(idx => idx.name === "$**_1");
assert.eq(1, indexes.length);
const indexSpec = indexes[0];
assert.eq(false, indexSpec.wildcardProjection.name, indexes);
assert.eq(false, indexSpec.wildcardProjection.type, indexes);
assert.eq(true, indexSpec.wildcardProjection._id, indexes);

coll.insert({name: "Ted", type: "Person", _id: 1});
coll.insert({name: "Bernard", type: "Person", _id: 2});

// Ensure we use the index for _id if we supply a hint.
const hintExplainRes = coll.find({_id: {$eq: 1}}).hint("$**_1").explain();
const winningPlan = getWinningPlan(getQueryPlanner(hintExplainRes));
assert.eq(winningPlan.inputStage.stage, "IXSCAN", winningPlan.inputStage);
assert.eq(winningPlan.inputStage.keyPattern, {$_path: 1, _id: 1}, winningPlan.inputStage);

// Test that the results are correct.
const hintedResults = coll.find({_id: {$eq: 1}}).hint("$**_1").toArray();
assert.eq(hintedResults.length, 1, hintedResults);
assert.eq(hintedResults[0]._id, 1, hintedResults);
