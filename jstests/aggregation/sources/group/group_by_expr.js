/*
 * Test that $group works with $expr in _id and accumulator fields.
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert([{_id: 1, x: 1, y: 5}, {_id: 2, x: 2, y: 10}]));

// Test $expr in the '_id' field.
// '_id' with empty $expr.
let results = coll.aggregate([{$group: {_id: {$expr: {}}}}]).toArray();
assert(resultsEq([{_id: {}}], results), results);

// '_id' with top-level $expr and field name (error case).
assert.throwsWithCode(
    () => coll.aggregate([{$group: {_id: {$expr: {$eq: ["$x", 1]}, secondGroupByField: "$y"}}}]),
    15983);
assert.throwsWithCode(
    () => coll.aggregate([{$group: {_id: {secondGroupByField: "$y", $expr: {$eq: ["$x", 1]}}}}]),
    16410);

// '_id' with nested $expr and field name.
results = coll.aggregate([{
                  $group: {
                      _id: {
                          exprFieldName: {$expr: {$max: [{$expr: {$multiply: ["$x", 2]}}, 3]}},
                          secondGroupByField: "$y"
                      }
                  }
              }])
              .toArray();
assert(resultsEq(
           [
               {_id: {exprFieldName: 3, secondGroupByField: 5}},
               {_id: {exprFieldName: 4, secondGroupByField: 10}}
           ],
           results),
       results);

// '_id' with non-empty constant scalar $expr.
results = coll.aggregate([{$group: {_id: {$expr: {$literal: "this is a string"}}}}]).toArray();
assert(resultsEq([{_id: "this is a string"}], results), results);

// '_id' with non-empty constant object $expr.
results = coll.aggregate([{$group: {_id: {$expr: {a: "b"}}}}]).toArray();
assert(resultsEq([{_id: {a: "b"}}], results), results);

// '_id' with non-empty non-constant $expr.
results = coll.aggregate([{$group: {_id: {$expr: {$multiply: ["$x", 10]}}}}]).toArray();
assert(resultsEq([{_id: 10}, {_id: 20}], results), results);

// Test $expr in an accumulator.

// $sum with empty $expr.
results = coll.aggregate([{$group: {_id: 1, sum: {$sum: {$expr: {}}}}}]).toArray();
assert(resultsEq([{_id: 1, sum: 0}], results), results);

// $sum with $expr and field name (error case).
assert.throwsWithCode(
    () => coll.aggregate(
        [{$group: {_id: 1, sum: {$sum: {$expr: {$eq: ["$x", 1]}, secondAccumulatorField: "$y"}}}}]),
    15983);
assert.throwsWithCode(
    () => coll.aggregate(
        [{$group: {_id: 1, sum: {$sum: {secondAccumulatorField: "$y", $expr: {$eq: ["$x", 1]}}}}}]),
    16410);

// $sum with non-empty constant scalar $expr.
results = coll.aggregate([{$group: {_id: 1, sum: {$sum: {$expr: {$literal: 5}}}}}]).toArray();
assert(resultsEq([{_id: 1, sum: 10}], results), results);

// $sum with non-empty constant object $expr.
results = coll.aggregate([{$group: {_id: 1, sum: {$sum: {$expr: {a: 5}}}}}]).toArray();
assert(resultsEq([{_id: 1, sum: 0}], results), results);

// $sum with non-empty non-constant $expr.
results =
    coll.aggregate([{$group: {_id: 1, sum: {$sum: {$expr: {$add: ["$x", "$y"]}}}}}]).toArray();
assert(resultsEq([{_id: 1, sum: 18}], results), results);

// $sum with non-constant $expr with nested $expr.
results =
    coll.aggregate([{
            $group: {_id: 1, sum: {$sum: {$expr: {$add: ["$x", {$expr: {$multiply: ["$y", 2]}}]}}}}
        }])
        .toArray();
assert(resultsEq([{_id: 1, sum: 33}], results), results);
