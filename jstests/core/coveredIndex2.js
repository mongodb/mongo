t = db["jstests_coveredIndex2"];
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

t.save({a: 1});
t.save({a: 2});
assert.eq(t.findOne({a: 1}).a, 1, "Cannot find right record");
assert.eq(t.count(), 2, "Not right length");

// use simple index
t.ensureIndex({a: 1});
var plan = t.find({a: 1}).explain();
assert(!isIndexOnly(plan.queryPlanner.winningPlan),
       "Find using covered index but all fields are returned");
var plan = t.find({a: 1}, {a: 1}).explain();
assert(!isIndexOnly(plan.queryPlanner.winningPlan), "Find using covered index but _id is returned");
var plan = t.find({a: 1}, {a: 1, _id: 0}).explain();
assert(isIndexOnly(plan.queryPlanner.winningPlan), "Find is not using covered index");

// add multikey
t.save({a: [3, 4]});
var plan = t.find({a: 1}, {a: 1, _id: 0}).explain();
assert(!isIndexOnly(plan.queryPlanner.winningPlan),
       "Find is using covered index even after multikey insert");
