// Cannot implicitly shard accessed collections because queries on a sharded collection are not
// able to be covered when they aren't on the shard key since the document needs to be fetched in
// order to apply the SHARDING_FILTER stage.
// @tags: [
//   uses_explain,
//   assumes_unsharded_collection,
//   # Time series collections do not support indexing array values in measurement fields.
//   exclude_from_timeseries_crud_passthrough,
//   # Plan shape assertions here depend on multikey filter pushdown, which older binaries lack.
//   requires_fcv_90,
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
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

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
explainRes = coll
    .explain("queryPlanner")
    .find({a: 1, b: 1}, {_id: 0, c: 1, d: 1})
    .sort({c: -1, d: -1})
    .finish();
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
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}, add2dsphereVersionIfNeeded()));
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
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}, add2dsphereVersionIfNeeded()));
assert.commandWorked(coll.insert({a: [], c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));

// Verify that a single-element array makes a 2dsphere index multikey.
coll = db.covered_multikey_2dsphere_single_element_array;
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1, c: "2dsphere"}, add2dsphereVersionIfNeeded()));
assert.commandWorked(coll.insert({a: {b: [3]}, c: {type: "Point", coordinates: [0, 0]}}));
explainRes = coll.explain().find().hint({"a.b": 1, c: "2dsphere"}).finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));

// Doc 0's "b" array has one value that matches the filter ("orange") and one that doesn't
// ("apple"). Verify the document is still returned, and that the filter can be pushed onto the
// index scan without a fetch.
coll = db.covered_multikey_inexact_covered_filter;
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1, b: ["apple", "orange"]}));
assert.commandWorked(coll.insert({_id: 1, a: 2, b: ["apple"]}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

assert.eq([{a: 1}], coll.find({a: 1, b: /orange/}, {_id: 0, a: 1}).toArray());
explainRes = coll
    .explain("queryPlanner")
    .find({a: 1, b: /orange/}, {_id: 0, a: 1})
    .finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(!planHasStage(db, winningPlan, "FETCH"));
assert(isIxscanMultikey(winningPlan));
ixscanStage = getPlanStage(winningPlan, "IXSCAN");
assert.hasFields(ixscanStage, ["filter"], "No filter found on IXSCAN: " + tojson(ixscanStage));
assert.docEq({b: {$regex: "orange"}}, ixscanStage.filter, "Incorrect filter on IXSCAN.");

// Same as above, but the multikey path traverses two nested arrays ("a" and "a.b") instead of a
// single flat array. Doc 0 has one key ("orange") that matches among several generated keys, and
// should be returned exactly once. A path with more than one array component always requires a
// FETCH (even for an exact-match query with no filter), so we only check the filter is pushed.
coll = db.covered_multikey_inexact_covered_filter_nested_arrays;
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [{b: [{c: "apple"}, {c: "orange"}]}, {c: "grape"}]}));
assert.commandWorked(coll.insert({_id: 1, a: [{b: [{c: "apple"}]}]}));
assert.commandWorked(coll.createIndex({"a.b.c": 1}));

assert.eq(
    [0],
    coll
        .find({"a.b.c": /orange/}, {_id: 1})
        .toArray()
        .map((doc) => doc._id),
);

explainRes = coll
    .explain("queryPlanner")
    .find({"a.b.c": /orange/})
    .finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));
ixscanStage = getPlanStage(winningPlan, "IXSCAN");
assert.hasFields(ixscanStage, ["filter"], "No filter found on IXSCAN: " + tojson(ixscanStage));
assert.docEq({"a.b.c": {$regex: "orange"}}, ixscanStage.filter, "Incorrect filter on IXSCAN.");

// $or over a multikey field, mixing an EXACT clause with an INEXACT_COVERED (regex) clause.
// Regression coverage for the multikey guard relaxation on the $or code path (as opposed to the
// $and/single-predicate paths already covered above).
coll = db.covered_multikey_or_inexact_covered;
coll.drop();
assert.commandWorked(coll.insert({_id: 0, names: ["david", "dave"]}));
assert.commandWorked(coll.insert({_id: 1, names: ["joseph", "joe", "joey"]}));
assert.commandWorked(coll.insert({_id: 2, names: ["mary"]}));
assert.commandWorked(coll.createIndex({names: 1}));

assert.eq(
    [0, 1],
    coll
        .find({$or: [{names: "dave"}, {names: /joe/}]}, {_id: 1})
        .sort({_id: 1})
        .toArray()
        .map((doc) => doc._id),
);

explainRes = coll
    .explain("queryPlanner")
    .find({$or: [{names: "dave"}, {names: /joe/}]})
    .finish();
winningPlan = getWinningPlanFromExplain(explainRes);
assert(isIxscan(db, winningPlan));
assert(isIxscanMultikey(winningPlan));
