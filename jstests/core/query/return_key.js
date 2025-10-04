// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

/**
 * Tests for returnKey.
 */
import {isIndexOnly} from "jstests/libs/query/analyze_plan.js";

let results;
let explain;

let coll = db.jstests_returnkey;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 2}));
assert.commandWorked(coll.insert({a: 3, b: 1}));

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Basic returnKey.
results = coll.find().hint({a: 1}).sort({a: 1}).returnKey().toArray();
assert.eq(results, [{a: 1}, {a: 2}, {a: 3}]);
results = coll.find().hint({a: 1}).sort({a: -1}).returnKey().toArray();
assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);

// Check that the plan is covered.
explain = coll.find().hint({a: 1}).sort({a: 1}).returnKey().explain();
assert(isIndexOnly(db, explain.queryPlanner.winningPlan));
explain = coll.find().hint({a: 1}).sort({a: -1}).returnKey().explain();
assert(isIndexOnly(db, explain.queryPlanner.winningPlan));

// returnKey with an in-memory sort.
results = coll.find().hint({a: 1}).sort({b: 1}).returnKey().toArray();
assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
results = coll.find().hint({a: 1}).sort({b: -1}).returnKey().toArray();
assert.eq(results, [{a: 1}, {a: 2}, {a: 3}]);

// Check that the plan is not covered.
explain = coll.find().hint({a: 1}).sort({b: 1}).returnKey().explain();
assert(!isIndexOnly(db, explain.queryPlanner.winningPlan));
explain = coll.find().hint({a: 1}).sort({b: -1}).returnKey().explain();
assert(!isIndexOnly(db, explain.queryPlanner.winningPlan));

// returnKey takes precedence over other a regular inclusion projection. Should still be
// covered.
results = coll.find({}, {b: 1}).hint({a: 1}).sort({a: -1}).returnKey().toArray();
assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
explain = coll.find({}, {b: 1}).hint({a: 1}).sort({a: -1}).returnKey().explain();
assert(isIndexOnly(db, explain.queryPlanner.winningPlan));

// returnKey takes precedence over other a regular exclusion projection. Should still be
// covered.
results = coll.find({}, {a: 0}).hint({a: 1}).sort({a: -1}).returnKey().toArray();
assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
explain = coll.find({}, {a: 0}).hint({a: 1}).sort({a: -1}).returnKey().explain();
assert(isIndexOnly(db, explain.queryPlanner.winningPlan));

// Unlike other projections, sortKey meta-projection can co-exist with returnKey.
results = coll
    .find({}, {c: {$meta: "sortKey"}})
    .hint({a: 1})
    .sort({a: -1})
    .returnKey()
    .toArray();
assert.eq(results, [
    {a: 3, c: [3]},
    {a: 2, c: [2]},
    {a: 1, c: [1]},
]);

// returnKey with sortKey $meta where there is an in-memory sort.
results = coll
    .find({}, {c: {$meta: "sortKey"}})
    .hint({a: 1})
    .sort({b: 1})
    .returnKey()
    .toArray();
assert.eq(results, [
    {a: 3, c: [1]},
    {a: 2, c: [2]},
    {a: 1, c: [3]},
]);

// returnKey with multiple sortKey $meta projections.
results = coll
    .find({}, {c: {$meta: "sortKey"}, d: {$meta: "sortKey"}})
    .hint({a: 1})
    .sort({b: 1})
    .returnKey()
    .toArray();
assert.eq(results, [
    {a: 3, c: [1], d: [1]},
    {a: 2, c: [2], d: [2]},
    {a: 1, c: [3], d: [3]},
]);

// returnKey with a sortKey $meta projection on a nested field.
results = coll
    .find({}, {"c.d": {$meta: "sortKey"}})
    .hint({a: 1})
    .sort({b: 1})
    .returnKey()
    .toArray();
assert.eq(results, [
    {a: 3, c: {d: [1]}},
    {a: 2, c: {d: [2]}},
    {a: 1, c: {d: [3]}},
]);
