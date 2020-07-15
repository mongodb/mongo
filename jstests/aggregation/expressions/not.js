// Tests the behavior of $not when used in agg expressions.

(function() {
"use strict";

const coll = db.not_expr;
coll.drop();

// Testing behavior for basic cases.
assert.commandWorked(coll.insert([
    {_id: 0, x: 0},
    {_id: 1, x: 2},
    {_id: 2, x: false},
    {_id: 3, x: true},
    {_id: 4, x: null},
    {_id: 5, x: []},       // Empty array truthy, $not(x) should be false.
    {_id: 6, x: [false]},  // $not(x) should be false.
    {_id: 7, x: [true]},   // $not(x) should be false.
    {_id: 8, x: "hello"},
    {_id: 9, x: ObjectId("5f0627282a63fc000b8fd067")},
    {_id: 10, x: ISODate("2021-01-01T00:00:00Z")},
    {_id: 11, x: {y: 2, z: 3}},
    {_id: 12}  // Missing field is falsy, so $not is true.
]));

let results = coll.aggregate([{$project: {x: {$not: "$x"}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: true},
    {_id: 1, x: false},
    {_id: 2, x: true},
    {_id: 3, x: false},
    {_id: 4, x: true},
    {_id: 5, x: false},
    {_id: 6, x: false},
    {_id: 7, x: false},
    {_id: 8, x: false},
    {_id: 9, x: false},
    {_id: 10, x: false},
    {_id: 11, x: false},
    {_id: 12, x: true}
]);

// Testing behavior for array of documents.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, x: [{y: 0}, {y: 1}]},  // x.y exists, so false.
    {_id: 1, x: [{z: 0}, {z: 1}]},  // x.y evaluates to [] which is truthy, so false.
    {_id: 2, x: [false]}            // x.y evaluates to [] which is truthy, so false
]));

results = coll.aggregate([{$project: {x: {$not: "$x.y"}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: false},
    {_id: 1, x: false},
    {_id: 2, x: false},
]);

// Testing behavior for nested documents.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, x: {y: 0}},
    {_id: 1, x: {y: 2}},
    {_id: 2, x: {y: false}},
    {_id: 3, x: {y: true}},
    {_id: 4, x: {y: null}},
    {_id: 5, x: {y: []}},       // $not(x.y) should be false.
    {_id: 6, x: {y: [false]}},  // $not(x.y) should be false.
    {_id: 7, x: {y: [true]}}    // $not(x.y) should be false.
]));

results = coll.aggregate([{$project: {x: {$not: "$x.y"}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: true},
    {_id: 1, x: false},
    {_id: 2, x: true},
    {_id: 3, x: false},
    {_id: 4, x: true},
    {_id: 5, x: false},
    {_id: 6, x: false},
    {_id: 7, x: false},
]);

// Testing behavior for other cases - nested $not, $and and $or & complex expressions.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, x: true, y: false},
    {_id: 1, x: false, y: true},
]));

results = coll.aggregate([{$project: {x: {$not: {$not: "$x"}}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: true},
    {_id: 1, x: false},
]);

results =
    coll.aggregate([{$project: {x: {$not: {$not: {$not: "$x"}}}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: false},
    {_id: 1, x: true},
]);

results =
    coll.aggregate([{$project: {x: {$not: {$and: ["$x", "$y"]}}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: true},
    {_id: 1, x: true},
]);

results =
    coll.aggregate([{$project: {x: {$not: {$or: ["$x", "$y"]}}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: false},
    {_id: 1, x: false},
]);

results =
    coll.aggregate([{$project: {x: {$and: [{$not: "$x"}, "$y"]}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: false},
    {_id: 1, x: true},
]);

results =
    coll.aggregate([{$project: {x: {$or: [{$not: "$x"}, "$y"]}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, x: false},
    {_id: 1, x: true},
]);

results = coll.aggregate([
                  {
                      $project: {
                          x: {
                              $switch: {
                                  branches: [
                                      {case: {$not: {$gt: ["$x", "$y"]}}, then: "x"},
                                      {case: {$not: {$lte: ["$x", "$y"]}}, then: "y"}
                                  ]
                              }
                          }
                      }
                  },
                  {$sort: {_id: 1}}
              ])
              .toArray();
assert.eq(results, [
    {_id: 0, x: "y"},
    {_id: 1, x: "x"},
]);
}());