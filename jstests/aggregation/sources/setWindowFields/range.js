/**
 * Test range-based window bounds.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db.setWindowFields_range;
coll.drop();

assert.commandWorked(coll.insert([
    {x: 0},
    {x: 1},
    {x: 1.5},
    {x: 2},
    {x: 3},
    {x: 100},
    {x: 100},
    {x: 101},
]));

// Make a setWindowFields stage with the given bounds.
function range(lower, upper) {
    return {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {x: 1},
            output: {
                y: {$push: "$x", window: {range: [lower, upper]}},
            }
        }
    };
}

// Run the pipeline, and unset _id.
function run(pipeline) {
    return coll
        .aggregate([
            ...pipeline,
            {$unset: '_id'},
        ])
        .toArray();
}

// The documents are not evenly spaced, so the window varies in size.
assert.sameMembers(run([range(-1, 0)]), [
    {x: 0, y: [0]},
    {x: 1, y: [0, 1]},
    {x: 1.5, y: [1, 1.5]},
    {x: 2, y: [1, 1.5, 2]},
    {x: 3, y: [2, 3]},
    // '0' means the current document and those that tie with it.
    {x: 100, y: [100, 100]},
    {x: 100, y: [100, 100]},
    {x: 101, y: [100, 100, 101]},
]);

// One or both endpoints can be unbounded.
assert.sameMembers(run([range('unbounded', 0)]), [
    {x: 0, y: [0]},
    {x: 1, y: [0, 1]},
    {x: 1.5, y: [0, 1, 1.5]},
    {x: 2, y: [0, 1, 1.5, 2]},
    {x: 3, y: [0, 1, 1.5, 2, 3]},
    // '0' means current document and those that tie with it.
    {x: 100, y: [0, 1, 1.5, 2, 3, 100, 100]},
    {x: 100, y: [0, 1, 1.5, 2, 3, 100, 100]},
    {x: 101, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
]);
assert.sameMembers(run([range(0, 'unbounded')]), [
    {x: 0, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 1, y: [1, 1.5, 2, 3, 100, 100, 101]},
    {x: 1.5, y: [1.5, 2, 3, 100, 100, 101]},
    {x: 2, y: [2, 3, 100, 100, 101]},
    {x: 3, y: [3, 100, 100, 101]},
    // '0' means current document and those that tie with it.
    {x: 100, y: [100, 100, 101]},
    {x: 100, y: [100, 100, 101]},
    {x: 101, y: [101]},
]);
assert.sameMembers(run([range('unbounded', 'unbounded')]), [
    {x: 0, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 1, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 1.5, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 2, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 3, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 100, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 100, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
    {x: 101, y: [0, 1, 1.5, 2, 3, 100, 100, 101]},
]);

// Unlike '0', 'current' always means the current document.
assert.sameMembers(run([range('current', 'current'), {$match: {x: 100}}]), [
    {x: 100, y: [100]},
    {x: 100, y: [100]},
]);
assert.sameMembers(run([range('current', +1), {$match: {x: 100}}]), [
    {x: 100, y: [100, 100, 101]},
    {x: 100, y: [100, 101]},
]);
assert.sameMembers(run([range(-97, 'current'), {$match: {x: 100}}]), [
    {x: 100, y: [3, 100]},
    {x: 100, y: [3, 100, 100]},
]);

// The window doesn't have to contain the current document.
// This also means the window can be empty.
assert.sameMembers(run([range(-1, -1)]), [
    // Near the partition boundary, no documents fall in the window.
    {x: 0, y: []},
    {x: 1, y: [0]},
    // The window can also be empty in the middle of a partition, because of gaps.
    // Here, the only value that would fit is 0.5, which doesn't occur.
    {x: 1.5, y: []},
    {x: 2, y: [1]},
    {x: 3, y: [2]},
    {x: 100, y: []},
    {x: 100, y: []},
    {x: 101, y: [100, 100]},
]);
assert.sameMembers(run([range(+1, +1)]), [
    {x: 0, y: [1]},
    {x: 1, y: [2]},
    {x: 1.5, y: []},
    {x: 2, y: [3]},
    {x: 3, y: []},
    {x: 100, y: [101]},
    {x: 100, y: [101]},
    {x: 101, y: []},
]);

// The window can be empty even if it's unbounded on one side.
assert.sameMembers(run([range('unbounded', -99)]), [
    {x: 0, y: []},
    {x: 1, y: []},
    {x: 1.5, y: []},
    {x: 2, y: []},
    {x: 3, y: []},
    {x: 100, y: [0, 1]},
    {x: 100, y: [0, 1]},
    {x: 101, y: [0, 1, 1.5, 2]},
]);
assert.sameMembers(run([range(+99, 'unbounded')]), [
    {x: 0, y: [100, 100, 101]},
    {x: 1, y: [100, 100, 101]},
    {x: 1.5, y: [101]},
    {x: 2, y: [101]},
    {x: 3, y: []},
    {x: 100, y: []},
    {x: 100, y: []},
    {x: 101, y: []},
]);

// Range-based windows reset between partitions.
assert.commandWorked(coll.updateMany({}, {$set: {partition: "A"}}));
assert.commandWorked(coll.insert([
    {partition: "B", x: 101},
    {partition: "B", x: 102},
    {partition: "B", x: 103},
]));
assert.sameMembers(run([range(-5, 0)]), [
    {partition: "A", x: 0, y: [0]},
    {partition: "A", x: 1, y: [0, 1]},
    {partition: "A", x: 1.5, y: [0, 1, 1.5]},
    {partition: "A", x: 2, y: [0, 1, 1.5, 2]},
    {partition: "A", x: 3, y: [0, 1, 1.5, 2, 3]},
    {partition: "A", x: 100, y: [100, 100]},
    {partition: "A", x: 100, y: [100, 100]},
    {partition: "A", x: 101, y: [100, 100, 101]},

    {partition: "B", x: 101, y: [101]},
    {partition: "B", x: 102, y: [101, 102]},
    {partition: "B", x: 103, y: [101, 102, 103]},
]);
assert.commandWorked(coll.deleteMany({partition: "B"}));
assert.commandWorked(coll.updateMany({}, [{$unset: 'partition'}]));

// Empty window vs no window:
// If no documents fall in the window, we evaluate the accumulator on zero documents.
// This makes sense for $push (and $sum), which has an identity element.
// But if the current document's sortBy is non-numeric, we really can't define a window at all,
// so it's an error.
assert.sameMembers(run([range(+999, +999)]), [
    {x: 0, y: []},
    {x: 1, y: []},
    {x: 1.5, y: []},
    {x: 2, y: []},
    {x: 3, y: []},
    {x: 100, y: []},
    {x: 100, y: []},
    {x: 101, y: []},
]);
coll.insert([
    {},
    {x: null},
    {x: ''},
    {x: {}},
]);
let error;
error = assert.throws(() => run([range(+999, +999)]));
assert.includes(error.message, 'Invalid range: Expected the sortBy field to be a number');
error = assert.throws(() => run([range(-999, +999)]));
assert.includes(error.message, 'Invalid range: Expected the sortBy field to be a number');
error = assert.throws(() => run([range('unbounded', 'unbounded')]));
assert.includes(error.message, 'Invalid range: Expected the sortBy field to be a number');

// Another case, involving ties and expiration.
coll.drop();
coll.insert([
    {x: 0},
    {x: 0},
    {x: 0},
    {x: 0},
    {x: 3},
    {x: 3},
    {x: 3},
]);
assert.sameMembers(run([range('unbounded', -3)]), [
    {x: 0, y: []},
    {x: 0, y: []},
    {x: 0, y: []},
    {x: 0, y: []},
    {x: 3, y: [0, 0, 0, 0]},
    {x: 3, y: [0, 0, 0, 0]},
    {x: 3, y: [0, 0, 0, 0]},
]);

// Test variable evaluation for input expressions.
assert.sameMembers(
    run([
        {
            $setWindowFields: {
                partitionBy: "$partition",
                sortBy: {x: 1},
                output: {
                    y: {
                        $sum: {$filter: {input: [], as: 'num', cond: {$gte: ['$$num', 2]}}},
                        window: {range: [-1, 1]}
                    },
                }
            }
        },
        {$unset: '_id'}
    ]),
    [
        {x: 0, y: 0},
        {x: 0, y: 0},
        {x: 0, y: 0},
        {x: 0, y: 0},
        {x: 3, y: 0},
        {x: 3, y: 0},
        {x: 3, y: 0},
    ]);

// Test that all values in the executors are cleared between partitions.
coll.drop();

// Create values such that not all will be removed from the first partition and one will be removed
// from the second.
assert.commandWorked(coll.insert([
    {partitionBy: 1, time: new Date(2020, 1, 1, 0, 30, 0, 0), temp: 10},
    {partitionBy: 1, time: new Date(2020, 1, 1, 1, 31, 0, 0), temp: 11},
    {partitionBy: 1, time: new Date(2020, 1, 1, 1, 32, 0, 0), temp: 12},
    {partitionBy: 1, time: new Date(2020, 1, 1, 1, 33, 0, 0), temp: 13},
    {partitionBy: 2, time: new Date(2020, 1, 1, 2, 31, 0, 0), temp: 5},
    {partitionBy: 2, time: new Date(2020, 1, 1, 2, 35, 0, 0), temp: 6},
    {partitionBy: 2, time: new Date(2020, 1, 1, 3, 34, 0, 0), temp: 2},
]));

const pipeline = [{
    $setWindowFields: {
        partitionBy: "$partitionBy",
        sortBy: {time: 1},
        output: {min: {$min: "$temp", window: {range: [-1, 0], unit: "hour"}}}
    }
}];
assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}));
})();
