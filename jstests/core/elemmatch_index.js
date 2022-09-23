/**
 * Test that queries containing $elemMatch correctly use an index if each child expression is
 * compatible with the index.
 * @tags: [
 *   assumes_balancer_off,
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.elemMatch_index;
coll.drop();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: [{}]}));
assert.commandWorked(coll.insert({a: [1, null]}));
assert.commandWorked(coll.insert({a: [{type: "Point", coordinates: [0, 0]}]}));

assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));

function assertIndexResults(coll, query, useIndex, nReturned) {
    const explainPlan = coll.find(query).explain("executionStats");
    assert.eq(isIxscan(db, getWinningPlan(explainPlan.queryPlanner)), useIndex);
    assert.eq(explainPlan.executionStats.nReturned, nReturned);
}

assertIndexResults(coll, {a: {$elemMatch: {$exists: false}}}, false, 0);

// An $elemMatch predicate is treated as nested, and the index should be used for $exists:true.
assertIndexResults(coll, {a: {$elemMatch: {$exists: true}}}, true, 3);

// $not within $elemMatch should not attempt to use a sparse index for $exists:false.
assertIndexResults(coll, {a: {$elemMatch: {$not: {$exists: false}}}}, false, 3);
assertIndexResults(coll, {a: {$elemMatch: {$gt: 0, $not: {$exists: false}}}}, false, 1);

// $geo within $elemMatch should not attempt to use a non-geo index.
assertIndexResults(coll,
                   {
                       a: {
                           $elemMatch: {
                               $geoWithin: {
                                   $geometry: {
                                       type: "Polygon",
                                       coordinates: [[[0, 0], [0, 1], [1, 0], [0, 0]]]
                                   }
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

(function() {
assert(coll.drop());
assert.commandWorked(coll.insert({a: [{b: {c: "x"}}]}));
assert.commandWorked(coll.createIndex({"a.b.c": 1}));

// Tests $elemMatch with path components that are empty strings. The system should not attempt to
// use the index for these queries.
assertIndexResults(coll, {"": {$elemMatch: {"a.b.c": "x"}}}, false, 0);
assertIndexResults(coll, {"": {$all: [{$elemMatch: {"a.b.c": "x"}}]}}, false, 0);
assertIndexResults(coll, {a: {$elemMatch: {"": {$elemMatch: {"b.c": "x"}}}}}, false, 0);

// Tests $elemMatch with supporting index and no path components that are empty strings.
assertIndexResults(coll, {a: {$elemMatch: {"b.c": "x"}}}, true, 1);
assertIndexResults(coll, {a: {$all: [{$elemMatch: {"b.c": "x"}}]}}, true, 1);
})();

(function() {
const coll = db.index_elemmatch1;
coll.drop();

let x = 0;
let y = 0;
const bulk = coll.initializeUnorderedBulkOp();
for (let a = 0; a < 10; a++) {
    for (let b = 0; b < 10; b++) {
        bulk.insert({a: a, b: b % 10, arr: [{x: x++ % 10, y: y++ % 10}]});
    }
}
assert.commandWorked(bulk.execute());

assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({"arr.x": 1, a: 1}));

const query = {
    a: 5,
    b: {$in: [1, 3, 5]},
    arr: {$elemMatch: {x: 5, y: 5}}
};

const count = coll.find(query).itcount();
assert.eq(count, 1);

const explain = coll.find(query).hint({"arr.x": 1, a: 1}).explain("executionStats");
assert.commandWorked(explain);
assert.eq(count, explain.executionStats.totalKeysExamined, explain);
})();
})();
