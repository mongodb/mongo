// $group has inconsistent behavior when differentiating between null and missing values, provided
// this test passes. Here, we check the cases where it is correct, and those where it is currently
// incorrect.
//
// This test issues some pipelines where it assumes an initial $sort will be absorbed and be
// covered, which will not happen if the $sort is within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
load('jstests/aggregation/extras/utils.js');  // For resultsEq.

(function() {
    "use strict";

    var coll = db.groupMissing;
    coll.drop();

    coll.insert({a: null});
    coll.insert({});

    var res = coll.aggregate({$group: {_id: "$a"}});
    var arr = res.toArray();
    assert.eq(arr.length, 1);
    assert.eq(arr[0]._id, null);

    coll.createIndex({a: 1});
    res = coll.aggregate({$sort: {a: 1}}, {$group: {_id: "$a"}});
    arr = res.toArray();
    assert.eq(arr.length, 1);
    assert.eq(arr[0]._id, null);

    coll.drop();

    coll.insert({a: null});
    coll.insert({});

    // Bug, see SERVER-21992.
    res = coll.aggregate({$group: {_id: {a: "$a"}}});
    assert(resultsEq(res.toArray(), [{_id: {a: null}}]));

    // Correct behavior after SERVER-21992 is fixed.
    if (0) {
        res = coll.aggregate({$group: {_id: {a: "$a"}}});
        assert(resultsEq(res.toArray(), [{_id: {a: null}}, {_id: {a: {}}}]));
    }

    // Bug, see SERVER-21992.
    coll.createIndex({a: 1});
    res = coll.aggregate({$group: {_id: {a: "$a"}}});
    assert(resultsEq(res.toArray(), [{_id: {a: null}}]));

    // Correct behavior after SERVER-21992 is fixed.
    if (0) {
        res = coll.aggregate({$group: {_id: {a: "$a"}}});
        assert(resultsEq(res.toArray(), [{_id: {a: null}}, {_id: {a: {}}}]));
    }

    coll.drop();
    coll.insert({a: null, b: 1});
    coll.insert({b: 1});
    coll.insert({a: null, b: 1});

    res = coll.aggregate({$group: {_id: {a: "$a", b: "$b"}}});
    assert(resultsEq(res.toArray(), [{_id: {b: 1}}, {_id: {a: null, b: 1}}]));

    // Bug, see SERVER-23229.
    coll.createIndex({a: 1, b: 1});
    res = coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}});
    assert(resultsEq(res.toArray(), [{_id: {a: null, b: 1}}]));

    // Correct behavior after SERVER-23229 is fixed.
    if (0) {
        coll.createIndex({a: 1, b: 1});
        res = coll.aggregate({$sort: {a: 1, b: 1}}, {$group: {_id: {a: "$a", b: "$b"}}});
        assert(resultsEq(res.toArray(), [{_id: {b: 1}}, {_id: {a: null, b: 1}}]));
    }
}());
