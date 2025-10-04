// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
//   requires_fastcount,
//   # The test assumes it is in control of which indexes exist and makes some assertions on explain
//   # plans.
//   assumes_no_implicit_index_creation,
// ]
// Include helpers for analyzing explain output.
import {getWinningPlanFromExplain, isIndexOnly} from "jstests/libs/query/analyze_plan.js";

const t = db["jstests_coveredIndex2"];
t.drop();

assert.commandWorked(t.insert({a: 1}));
assert.commandWorked(t.insert({a: 2}));
assert.eq(t.findOne({a: 1}).a, 1, "Cannot find right record");
assert.eq(t.count(), 2, "Not right length");

// use simple index
assert.commandWorked(t.createIndex({a: 1}));
let plan = t.find({a: 1}).explain();
assert(!isIndexOnly(db, getWinningPlanFromExplain(plan)), "Find using covered index but all fields are returned");
plan = t.find({a: 1}, {a: 1}).explain();
assert(!isIndexOnly(db, getWinningPlanFromExplain(plan)), "Find using covered index but _id is returned");
plan = t.find({a: 1}, {a: 1, _id: 0}).explain();
assert(isIndexOnly(db, getWinningPlanFromExplain(plan)), "Find is not using covered index");

// add multikey
assert.commandWorked(t.insert({a: [3, 4]}));
plan = t.find({a: 1}, {a: 1, _id: 0}).explain();
assert(!isIndexOnly(db, getWinningPlanFromExplain(plan)), "Find is using covered index even after multikey insert");
