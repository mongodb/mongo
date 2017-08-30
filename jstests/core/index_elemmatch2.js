/**
 * Test that queries containing $elemMatch correctly use an index if each child expression is
 * compatible with the index.
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const coll = db.elemMatch_index;
    coll.drop();

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: [{}]}));
    assert.writeOK(coll.insert({a: [1, null]}));
    assert.writeOK(coll.insert({a: [{type: "Point", coordinates: [0, 0]}]}));

    assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));

    function assertIndexResults(coll, query, useIndex, nReturned) {
        const explainPlan = coll.find(query).explain("executionStats");
        assert.eq(isIxscan(explainPlan.queryPlanner.winningPlan), useIndex);
        assert.eq(explainPlan.executionStats.nReturned, nReturned);
    }

    assertIndexResults(coll, {a: {$elemMatch: {$exists: false}}}, false, 0);

    // An $elemMatch predicate is treated as nested, and the index should be used for $exists:true.
    assertIndexResults(coll, {a: {$elemMatch: {$exists: true}}}, true, 3);

    // $not within $elemMatch should not attempt to use a sparse index for $exists:false.
    assertIndexResults(coll, {a: {$elemMatch: {$not: {$exists: false}}}}, false, 3);
    assertIndexResults(coll, {a: {$elemMatch: {$gt: 0, $not: {$exists: false}}}}, false, 1);

    // $geo within $elemMatch should not attempt to use a non-geo index.
    assertIndexResults(
        coll,
        {
          a: {
              $elemMatch: {
                  $geoWithin: {
                      $geometry:
                          {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 0], [0, 0]]]}
                  }
              }
          }
        },
        false,
        1);

    // $in with a null value within $elemMatch should use a sparse index.
    assertIndexResults(coll, {a: {$elemMatch: {$in: [null]}}}, true, 1);

    // $eq with a null value within $elemMatch should use a sparse index.
    assertIndexResults(coll, {a: {$elemMatch: {$eq: null}}}, true, 1);

    // A negated regex within $elemMatch should not use an index, sparse or not.
    assertIndexResults(coll, {a: {$elemMatch: {$not: {$in: [/^a/]}}}}, false, 3);

    coll.dropIndexes();
    assert.commandWorked(coll.createIndex({a: 1}));
    assertIndexResults(coll, {a: {$elemMatch: {$not: {$in: [/^a/]}}}}, false, 3);
})();
