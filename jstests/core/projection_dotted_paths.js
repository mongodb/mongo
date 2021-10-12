// Failing due to queries on a sharded collection not able to be covered when they aren't on the
// shard key since the document needs to be fetched in order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

/**
 * Test projections with dotted field paths. Also test that such projections result in covered plans
 * when appropriate.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/aggregation/extras/utils.js");  // arrayEq
load("jstests/libs/sbe_explain_helpers.js");

let coll = db["projection_dotted_paths"];
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1, "b.d": 1, c: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 1, b: {c: 1, d: 1, e: 1}, c: 1, e: 1}));

// Project exactly the set of fields in the index. Verify that the projection is computed
// correctly and that the plan is covered.
let resultDoc = coll.findOne({a: 1}, {_id: 0, a: 1, "b.c": 1, "b.d": 1, c: 1});
assert.eq(resultDoc, {a: 1, b: {c: 1, d: 1}, c: 1});
let explain = coll.find({a: 1}, {_id: 0, a: 1, "b.c": 1, "b.d": 1, c: 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), explain);
assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)), explain);

// Project a subset of the indexed fields. Verify that the projection is computed correctly and
// that the plan is covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, c: 1});
assert.eq(resultDoc, {b: {c: 1}, c: 1});
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, c: 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// Project exactly the set of fields in the index but also include _id. Verify that the
// projection is computed correctly and that the plan cannot be covered.
resultDoc = coll.findOne({a: 1}, {_id: 1, a: 1, "b.c": 1, "b.d": 1, c: 1});
assert.docEq(resultDoc, {_id: 1, a: 1, b: {c: 1, d: 1}, c: 1});
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, c: 1}).explain("queryPlanner");
explain = coll.find({a: 1}, {_id: 1, a: 1, "b.c": 1, "b.d": 1, c: 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// Project a not-indexed field that exists in the collection. The plan should not be covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1});
assert.docEq(resultDoc, {b: {c: 1, e: 1}, c: 1});
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// Project a not-indexed field that does not exist in the collection. The plan should not be
// covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, "b.z": 1, c: 1});
assert.docEq(resultDoc, {b: {c: 1}, c: 1});
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, "b.z": 1, c: 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// Verify that the correct projection is computed with an idhack query.
resultDoc = coll.findOne({_id: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1});
assert.docEq(resultDoc, {b: {c: 1, e: 1}, c: 1});
explain = coll.find({_id: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1}).explain("queryPlanner");

engineSpecificAssertion(isIdhack(db, getWinningPlan(explain.queryPlanner)),
                        isIxscan(db, getWinningPlan(explain.queryPlanner)),
                        db,
                        explain);

// If we make a dotted path multikey, projections using that path cannot be covered. But
// projections which do not include the multikey path can still be covered.
assert.commandWorked(coll.insert({a: 2, b: {c: 1, d: [1, 2, 3]}}));

resultDoc = coll.findOne({a: 2}, {_id: 0, "b.c": 1, "b.d": 1});
assert.eq(resultDoc, {b: {c: 1, d: [1, 2, 3]}});
explain = coll.find({a: 2}, {_id: 0, "b.c": 1, "b.d": 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

resultDoc = coll.findOne({a: 2}, {_id: 0, "b.c": 1});
assert.eq(resultDoc, {b: {c: 1}});
explain = coll.find({a: 2}, {_id: 0, "b.c": 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
// Path-level multikey info allows for generating a covered plan.
assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// Verify that dotted projections work for multiple levels of nesting.
assert.commandWorked(coll.createIndex({a: 1, "x.y.y": 1, "x.y.z": 1, "x.z": 1}));
assert.commandWorked(coll.insert({a: 3, x: {y: {y: 1, f: 1, z: 1}, f: 1, z: 1}}));
resultDoc = coll.findOne({a: 3}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1});
assert.eq(resultDoc, {x: {y: {y: 1, z: 1}, z: 1}});
explain = coll.find({a: 3}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1}).explain("queryPlanner");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));

// If projected nested paths do not exist in the indexed document, then they will get filled in
// with nulls. This is a bug tracked by SERVER-23229.
resultDoc = coll.findOne({a: 1}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1});
assert.eq(resultDoc, {x: {y: {y: null, z: null}, z: null}});

// Test covered plans where an index contains overlapping dotted paths.
{
    assert.commandWorked(coll.createIndex({"a.b.c": 1, "a.b": 1}));
    assert.commandWorked(coll.insert({a: {b: {c: 1, d: 1}}}));
    explain = coll.find({"a.b.c": 1}, {_id: 0, "a.b": 1}).explain();
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
    assert.eq(coll.findOne({"a.b.c": 1}, {_id: 0, "a.b": 1}), {a: {b: {c: 1, d: 1}}});
}

{
    assert.commandWorked(coll.dropIndexes());

    assert.commandWorked(coll.createIndex({"a.b": 1, "a.b.c": 1}));
    assert.commandWorked(coll.insert({a: {b: {c: 1, d: 1}}}));

    const filter = {"a.b": {c: 1, d: 1}};
    explain = coll.find(filter, {_id: 0, "a.b": 1}).explain();
    assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
    assert.eq(coll.findOne(filter, {_id: 0, "a.b": 1}), {a: {b: {c: 1, d: 1}}});
}

{
    assert(coll.drop());

    assert.commandWorked(coll.insert({a: {x: 1, b: {x: 2}}, b: {c: 3}}));

    assert(arrayEq(coll.find({}, {_id: 0, "a": "$p", "b.c": "$q"}).toArray(), [{b: {}}]));
    assert(arrayEq(coll.find({}, {_id: 0, "a.x": "$a.x", "a.b.x": "$a.x"}).toArray(),
                   [{a: {x: 1, b: {x: 1}}}]));
}
}());
