// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

/**
 * Test covering behavior for queries over a multikey index.
 */
// For making assertions about explain output.
import {
    getPlanStage,
    getWinningPlanFromExplain,
    isCollscan,
    isIxscan,
    isIxscanMultikey,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

let coll = db.covered_multikey_compound_a_1_b_1;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: [2, 3, 4]}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

assert.eq(1, coll.find({a: 1, b: 2}, {_id: 0, a: 1}).itcount());
assert.eq({a: 1}, coll.findOne({a: 1, b: 2}, {_id: 0, a: 1}));
let explainRes = coll.explain("queryPlanner").find({a: 1, b: 2}, {_id: 0, a: 1}).finish();
let winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(!planHasStage(db, winningPlan, "FETCH"));

coll = db.covered_multikey_compound_abcd;
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: [1, 2, 3], c: 3, d: 5}));
assert.commandWorked(coll.insert({a: [1, 2, 3], b: 1, c: 4, d: 6}));
assert.commandWorked(coll.createIndex({a: 1, b: 1, c: -1, d: -1}));

let cursor = coll.find({a: 1, b: 1}, {_id: 0, c: 1, d: 1}).sort({c: -1, d: -1});
assert.eq(cursor.next(), {c: 4, d: 6});
assert.eq(cursor.next(), {c: 3, d: 5});
assert(!cursor.hasNext());
explainRes = coll.explain("queryPlanner").find({a: 1, b: 1}, {_id: 0, c: 1, d: 1}).sort({c: -1, d: -1}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(!planHasStage(db, winningPlan, "FETCH"));

// Verify that a query cannot be covered over a path which is multikey due to an empty array.
coll = db.covered_multikey_a_1_empty_array;
coll.drop();
assert.commandWorked(coll.insert({a: []}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.eq({a: []}, coll.findOne({a: []}, {_id: 0, a: 1}));
explainRes = coll.explain("queryPlanner").find({a: []}, {_id: 0, a: 1}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(planHasStage(db, winningPlan, "FETCH"));
assert(isIxscanMultikey(winningPlan));

// Verify that a query cannot be covered over a path which is multikey due to a single-element
// array.
coll = db.covered_multikey_a_1_single_element_array_insert;
coll.drop();
assert.commandWorked(coll.insert({a: [2]}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.eq({a: [2]}, coll.findOne({a: 2}, {_id: 0, a: 1}));
explainRes = coll.explain("queryPlanner").find({a: 2}, {_id: 0, a: 1}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(planHasStage(db, winningPlan, "FETCH"));
assert(isIxscanMultikey(winningPlan));

// Verify that a query cannot be covered over a path which is multikey due to a single-element
// array, where the path is made multikey by an update rather than an insert.
coll = db.covered_multikey_a_single_element_array_update;
coll.drop();
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.update({}, {$set: {a: [2]}}));
assert.eq({a: [2]}, coll.findOne({a: 2}, {_id: 0, a: 1}));
explainRes = coll.explain("queryPlanner").find({a: 2}, {_id: 0, a: 1}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(planHasStage(db, winningPlan, "FETCH"));
assert(isIxscanMultikey(winningPlan));

// Verify that a trailing empty array makes a 2dsphere index multikey.
coll = db.covered_multikey_2dsphere_empty_array_trailing;
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
assert.commandWorked(coll.insert({a: {b: 1}, c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
let ixscanStage = getPlanStage(winningPlan, "IXSCAN");
assert.neq(null, ixscanStage);
assert.eq(false, ixscanStage.isMultiKey);
assert.commandWorked(coll.insert({a: {b: []}, c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));

// Verify that a mid-path empty array makes a 2dsphere index multikey.
coll = db.covered_multikey_2dsphere_empty_array_midpath;
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
assert.commandWorked(coll.insert({a: [], c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));

// Verify that a single-element array makes a 2dsphere index multikey.
coll = db.covered_multikey_2dsphere_single_element_array;
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}));
assert.commandWorked(coll.insert({a: {b: [3]}, c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));
