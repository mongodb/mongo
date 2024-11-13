/*
 * Test partitioning inside $setWindowFields returns expected query results.
 */

import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
const collName = coll.getName();

coll.drop();
assert.commandWorked(coll.insertMany([
    {int_field: 0},
    {int_field: null, other_field: null},
    {other_field: 0},
    {int_field: null},
    {other_field: null},
    {int_field: null, other_field: null}
]));

// Test that missing and null field are in the same partition.
let res = coll.aggregate([
    {$setWindowFields: {partitionBy: "$int_field", output: {count: {$sum: 1}}}},
    {$project: {_id: 0}}
]);
assert(resultsEq(res.toArray(), [
    {int_field: null, other_field: null, count: 5},
    {other_field: 0, count: 5},
    {int_field: null, count: 5},
    {other_field: null, count: 5},
    {int_field: null, other_field: null, count: 5},
    {int_field: 0, count: 1}
]));

// Test that the compound key with the mix of missing and null field works correctly.
res = coll.aggregate([
    {
        $setWindowFields: {
            partitionBy: {int_field: "$int_field", other_field: "$other_field"},
            output: {count: {$sum: 1}}
        }
    },
    {$project: {_id: 0}}
]);
assert(resultsEq(res.toArray(), [
    {int_field: null, count: 1},
    {int_field: null, other_field: null, count: 2},
    {int_field: null, other_field: null, count: 2},
    {other_field: null, count: 1},
    {int_field: 0, count: 1},
    {other_field: 0, count: 1}
]));

// Test that 'partitionBy' that references a let variable and evaluates to a constant works
// correctly.
coll.drop();
coll.insert([
    {
        _id: 1,
    },
    {_id: 2, int_field: 3, other_field: 'x'}
]);
res = coll.aggregate(
    {
        $lookup: {
            from: collName,
            let: {v: "$int_field"},
            pipeline: [
                {
                    $fill: {
                        partitionBy: {$or: ["$$v", "$int_field"]},
                        sortBy: {int_field: -1},
                        output: {other_field: {method: 'locf'}}
                    }
                }
            ],
            as: "o"
        }
    }
);
assert(resultsEq(res.toArray(), [
    {_id: 1, o: [{_id: 1, other_field: null}, {_id: 2, int_field: 3, other_field: "x"}]},
    {
        _id: 2,
        int_field: 3,
        other_field: 'x',
        o: [{_id: 2, int_field: 3, other_field: "x"}, {_id: 1, other_field: "x"}]
    }
]));
