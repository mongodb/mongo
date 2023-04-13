/**
 * Basic tests for the $_internalEqHash match expression.
 * @tags: [
 *   # explain doesn't support read concern
 *   assumes_read_concern_unchanged,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For 'isCollscan()' and similar.
load("jstests/aggregation/extras/utils.js");  // For 'resultsEq().'

const coll = db.match_internal_eq_hash;

(function testTopLevel() {
    coll.drop();

    assert.commandWorked(coll.insert([
        {_id: 0},
        {_id: 1, a: 1},
        {_id: 2, a: NumberLong(1)},
        {_id: 3, a: "1"},
        {_id: 4, a: null}
    ]));

    // Test that the expression works without an index - just doesn't crash or match anything in
    // this case.
    assert.eq(coll.find({a: {$_internalEqHash: NumberLong(0)}}).toArray(),
              [],
              "Expected nothing to hash to 0");
    let explainPlan = coll.find({a: {$_internalEqHash: NumberLong(0)}}).explain();
    assert(isCollscan(db, explainPlan));

    // Test that the expression works with a hashed index.
    assert.commandWorked(coll.createIndex({a: "hashed"}));
    const doc = coll.findOne({a: 1}, {key: {$meta: "indexKey"}}, {hint: {a: "hashed"}});
    jsTestLog(coll.find({}, {key: {$meta: "indexKey"}}, 0, 0, {hint: {a: "hashed"}}).explain());
    jsTestLog(doc);
    const hashOfInterest = doc.key.a;

    const testQuery = {a: {$_internalEqHash: hashOfInterest}};
    assert(resultsEq(coll.find(testQuery).toArray(), [{_id: 1, a: 1}, {_id: 2, a: NumberLong(1)}]));
    explainPlan = coll.find(testQuery).explain();
    assert(isIxscan(db, explainPlan), explainPlan);

    // Make sure that multikey hashed indexes are not supported. If they are someday, then test
    // should be modified to ensure this expression still works.
    assert.commandFailedWithCode(coll.insert({a: [1]}), 16766);
    assert.commandFailedWithCode(coll.insert({a: [0, 1, 2, 3]}), 16766);

    // Now drop the index and use a collscan again - should get the same results.
    assert.commandWorked(coll.dropIndex({a: "hashed"}));
    explainPlan = coll.find(testQuery, {_id: 1}).explain();
    assert(isCollscan(db, explainPlan));
    assert(resultsEq(coll.find(testQuery).toArray(), [{_id: 1, a: 1}, {_id: 2, a: NumberLong(1)}]));

    // Now add a compound hashed index and test it can still work on a leading hash component.
    assert.commandWorked(coll.createIndex({a: "hashed", b: 1}));

    assert(resultsEq(coll.find(testQuery).toArray(), [{_id: 1, a: 1}, {_id: 2, a: NumberLong(1)}]));
    explainPlan = coll.find(testQuery).explain();
    assert(isIxscan(db, explainPlan), explainPlan);

    // Now add a compound hashed index and test it cannot work on a trailing hash component (could
    // not effectively seek).
    assert.commandWorked(coll.dropIndex({a: "hashed", b: 1}));
    assert.commandWorked(coll.createIndex({b: 1, a: "hashed"}));

    assert(resultsEq(coll.find(testQuery).toArray(), [{_id: 1, a: 1}, {_id: 2, a: NumberLong(1)}]));
    explainPlan = coll.find(testQuery).explain();
    assert(isCollscan(db, explainPlan), explainPlan);

    // Now add a non-hashed index and test it still works correctly but does not mistakenly try to
    // use the non-hashed index. It should still do a collection scan.
    assert.commandWorked(coll.dropIndex({b: 1, a: "hashed"}));
    assert.commandWorked(coll.createIndex({a: 1}));

    assert(resultsEq(coll.find(testQuery).toArray(), [{_id: 1, a: 1}, {_id: 2, a: NumberLong(1)}]));
    explainPlan = coll.find(testQuery).explain();
    assert(isCollscan(db, explainPlan), explainPlan);
}());

(function testDotted() {
    coll.drop();

    assert.commandWorked(coll.insert([
        {},
        {a: 1},
        {a: {}},
        {a: {b: 1}},
        {a: {b: {c: NumberDecimal("1.0")}}},
        {a: {b: {c: 2}}},
        {"a.b.c": 1},
    ]));

    assert.commandWorked(coll.createIndex({"a.b.c": "hashed"}));
    const hashOfInterest =
        coll.findOne({"a.b.c": 1}, {key: {$meta: "indexKey"}}, {hint: {"a.b.c": "hashed"}})
            .key["a.b.c"];
    const testQuery = {"a.b.c": {$_internalEqHash: hashOfInterest}};
    assert(
        resultsEq(coll.find(testQuery, {_id: 0}).toArray(), [{a: {b: {c: NumberDecimal("1.0")}}}]));

    let explainPlan = coll.find(testQuery).explain();
    assert(isIxscan(db, explainPlan), explainPlan);

    // Now drop the index and use a collscan again - should get the same results.
    assert.commandWorked(coll.dropIndex({"a.b.c": "hashed"}));
    explainPlan = coll.find(testQuery, {_id: 1}).explain();
    assert(isCollscan(db, explainPlan));
    assert(
        resultsEq(coll.find(testQuery, {_id: 0}).toArray(), [{a: {b: {c: NumberDecimal("1.0")}}}]));
}());

(function testInvalidTypes() {
    coll.drop();

    assert.commandWorked(coll.createIndex({a: "hashed"}));
    const invalidBsonTypes = [
        MinKey,
        NaN,
        -Infinity,
        0.0,
        Infinity,
        "not a number",
        {},
        {"embedded": "object"},
        [],
        [1],
        [1, 2, 3],
        null,
        BinData(0, 'asdf'),
        UUID("326d92af-2d76-452b-a03f-69f05ab98416"),
        undefined,
        ObjectId("62d05ec744ca83616c92772c"),
        false,
        true,
        ISODate("2023-03-28T18:34:28.937Z"),
        /a/,
        function inc(x) {
            return x + 1;
        },
        MaxKey,
    ];
    invalidBsonTypes.forEach(function(v) {
        assert.commandFailedWithCode(
            db.runCommand({find: "match_internal_eq_hash", filter: {a: {$_internalEqHash: v}}}), 2);
    });
})();
}());
