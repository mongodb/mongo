// Failing due to queries on a sharded collection not able to be covered when they aren't on the
// shard key since the document needs to be fetched in order to apply the SHARDING_FILTER stage.
// @tags: [
//   assumes_unsharded_collection,
// ]

/**
 * Test projections with dotted field paths. Also test that such projections result in covered plans
 * when appropriate.
 */
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {
    getOptimizer,
    getWinningPlan,
    isCollscan,
    isIdhackOrExpress,
    isIndexOnly,
    isIxscan
} from "jstests/libs/analyze_plan.js";

let coll = db["projection_dotted_paths"];
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1, "b.d": 1, c: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 1, b: {c: 1, d: 1, e: 1}, c: 1, e: 1}));

// Project exactly the set of fields in the index. Verify that the projection is computed
// correctly and that the plan is covered.
let resultDoc = coll.findOne({a: 1}, {_id: 0, a: 1, "b.c": 1, "b.d": 1, c: 1});
assert.eq(resultDoc, {a: 1, b: {c: 1, d: 1}, c: 1});
let explain = coll.find({a: 1}, {_id: 0, a: 1, "b.c": 1, "b.d": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), explain);
        assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)), explain);
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

// Project a subset of the indexed fields. Verify that the projection is computed correctly and
// that the plan is covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, c: 1});
assert.eq(resultDoc, {b: {c: 1}, c: 1});
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

// Project exactly the set of fields in the index but also include _id. Verify that the
// projection is computed correctly and that the plan cannot be covered.
resultDoc = coll.findOne({a: 1}, {_id: 1, a: 1, "b.c": 1, "b.d": 1, c: 1});
assert.docEq({_id: 1, a: 1, b: {c: 1, d: 1}, c: 1}, resultDoc);
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, c: 1}).explain("queryPlanner");
explain = coll.find({a: 1}, {_id: 1, a: 1, "b.c": 1, "b.d": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

// Project a not-indexed field that exists in the collection. The plan should not be covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1});
assert.docEq({b: {c: 1, e: 1}, c: 1}, resultDoc);
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

// Project a not-indexed field that does not exist in the collection. The plan should not be
// covered.
resultDoc = coll.findOne({a: 1}, {_id: 0, "b.c": 1, "b.z": 1, c: 1});
assert.docEq({b: {c: 1}, c: 1}, resultDoc);
explain = coll.find({a: 1}, {_id: 0, "b.c": 1, "b.z": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
}

// Verify that the correct projection is computed with an idhack query.
resultDoc = coll.findOne({_id: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1});
assert.docEq({b: {c: 1, e: 1}, c: 1}, resultDoc);
explain = coll.find({_id: 1}, {_id: 0, "b.c": 1, "b.e": 1, c: 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIdhackOrExpress(db, getWinningPlan(explain.queryPlanner)), explain);
        break;
    }
    case "CQF":
        // TODO SERVER-70847, how to recognize the case of an IDHACK for Bonsai?
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
}

// If we make a dotted path multikey, projections using that path cannot be covered. But
// projections which do not include the multikey path can still be covered.
assert.commandWorked(coll.insert({a: 2, b: {c: 1, d: [1, 2, 3]}}));

resultDoc = coll.findOne({a: 2}, {_id: 0, "b.c": 1, "b.d": 1});
assert.eq(resultDoc, {b: {c: 1, d: [1, 2, 3]}});
explain = coll.find({a: 2}, {_id: 0, "b.c": 1, "b.d": 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(!isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

resultDoc = coll.findOne({a: 2}, {_id: 0, "b.c": 1});
assert.eq(resultDoc, {b: {c: 1}});
explain = coll.find({a: 2}, {_id: 0, "b.c": 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        // Path-level multikey info allows for generating a covered plan.
        assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        assert(isCollscan(db, explain));
        break;
}

// Verify that dotted projections work for multiple levels of nesting.
assert.commandWorked(coll.createIndex({a: 1, "x.y.y": 1, "x.y.z": 1, "x.z": 1}));
assert.commandWorked(coll.insert({a: 3, x: {y: {y: 1, f: 1, z: 1}, f: 1, z: 1}}));
resultDoc = coll.findOne({a: 3}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1});
assert.eq(resultDoc, {x: {y: {y: 1, z: 1}, z: 1}});
explain = coll.find({a: 3}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1}).explain("queryPlanner");

switch (getOptimizer(explain)) {
    case "classic": {
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
        assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
        break;
    }
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
}

// If projected nested paths do not exist in the indexed document, then they will get filled in
// with nulls. This is a bug tracked by SERVER-23229.
resultDoc = coll.findOne({a: 1}, {_id: 0, "x.y.y": 1, "x.y.z": 1, "x.z": 1});
assert.eq(resultDoc, {x: {y: {y: null, z: null}, z: null}});

// Test covered plans where an index contains overlapping dotted paths.
{
    assert.commandWorked(coll.createIndex({"a.b.c": 1, "a.b": 1}));
    assert.commandWorked(coll.insert({a: {b: {c: 1, d: 1}}}));
    explain = coll.find({"a.b.c": 1}, {_id: 0, "a.b": 1}).explain();

    switch (getOptimizer(explain)) {
        case "classic": {
            assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
            assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
            break;
        }
        case "CQF":
            // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
            // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
            assert(isCollscan(db, explain));
            break;
    }
    assert.eq(coll.findOne({"a.b.c": 1}, {_id: 0, "a.b": 1}), {a: {b: {c: 1, d: 1}}});
}

{
    assert.commandWorked(coll.dropIndexes());

    assert.commandWorked(coll.createIndex({"a.b": 1, "a.b.c": 1}));
    assert.commandWorked(coll.insert({a: {b: {c: 1, d: 1}}}));

    const filter = {"a.b": {c: 1, d: 1}};
    explain = coll.find(filter, {_id: 0, "a.b": 1}).explain();

    switch (getOptimizer(explain)) {
        case "classic": {
            assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
            assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)));
            break;
        }
        case "CQF":
            // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
            // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
            assert(isCollscan(db, explain));
            break;
    }
    assert.eq(coll.findOne(filter, {_id: 0, "a.b": 1}), {a: {b: {c: 1, d: 1}}});
}

{
    assert(coll.drop());

    assert.commandWorked(coll.insert({a: {x: 1, b: {x: 2}}, b: {c: 3}}));

    assert(arrayEq(coll.find({}, {_id: 0, "a": "$p", "b.c": "$q"}).toArray(), [{b: {}}]));
    assert(arrayEq(coll.find({}, {_id: 0, "a.x": "$a.x", "a.b.x": "$a.x"}).toArray(),
                   [{a: {x: 1, b: {x: 1}}}]));
}
