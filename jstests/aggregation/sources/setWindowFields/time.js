/**
 * Test time-based window bounds.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const coll = db.setWindowFields_time;
coll.drop();

// Create an ISODate that occurs 'seconds' seconds after a fixed date.
function makeDate(seconds) {
    let result = ISODate('2021-01-01T00:00:00Z');
    result.setUTCMilliseconds(seconds * 1000);
    return result;
}
function makeDates(arrayOfSeconds) {
    return arrayOfSeconds.map(seconds => makeDate(seconds));
}

assert.commandWorked(coll.insert([
    {x: makeDate(0)},
    {x: makeDate(1)},
    {x: makeDate(1.5)},
    {x: makeDate(2)},
    {x: makeDate(3)},
    {x: makeDate(42)},
    {x: makeDate(42)},
    {x: makeDate(43)},
]));

// Make a setWindowFields stage with the given bounds.
function range(lower, upper, unit = 'second') {
    return {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {x: 1},
            output: {
                y: {$push: "$x", window: {range: [lower, upper], unit}},
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
    {x: makeDate(0), y: makeDates([0])},
    {x: makeDate(1), y: makeDates([0, 1])},
    {x: makeDate(1.5), y: makeDates([1, 1.5])},
    {x: makeDate(2), y: makeDates([1, 1.5, 2])},
    {x: makeDate(3), y: makeDates([2, 3])},
    // '0' means the current document and those that tie with it.
    {x: makeDate(42), y: makeDates([42, 42])},
    {x: makeDate(42), y: makeDates([42, 42])},
    {x: makeDate(43), y: makeDates([42, 42, 43])},
]);

// Bounds can be specified with different units.
assert.sameMembers(run([range(-1000, 0, 'millisecond')]), [
    {x: makeDate(0), y: makeDates([0])},
    {x: makeDate(1), y: makeDates([0, 1])},
    {x: makeDate(1.5), y: makeDates([1, 1.5])},
    {x: makeDate(2), y: makeDates([1, 1.5, 2])},
    {x: makeDate(3), y: makeDates([2, 3])},
    // '0' means the current document and those that tie with it.
    {x: makeDate(42), y: makeDates([42, 42])},
    {x: makeDate(42), y: makeDates([42, 42])},
    {x: makeDate(43), y: makeDates([42, 42, 43])},
]);

// Fractional units are not allowed.
let error;
error = assert.throws(() => run([range(-1.5, 0, 'second')]));
assert.commandFailedWithCode(error, ErrorCodes.FailedToParse);
assert.includes(error.message, 'range-based bounds must be an integer');

// One or both endpoints can be unbounded.
assert.sameMembers(run([range('unbounded', 0)]), [
    {x: makeDate(0), y: makeDates([0])},
    {x: makeDate(1), y: makeDates([0, 1])},
    {x: makeDate(1.5), y: makeDates([0, 1, 1.5])},
    {x: makeDate(2), y: makeDates([0, 1, 1.5, 2])},
    {x: makeDate(3), y: makeDates([0, 1, 1.5, 2, 3])},
    // '0' means current document and those that tie with it.
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2, 3, 42, 42])},
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2, 3, 42, 42])},
    {x: makeDate(43), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
]);
assert.sameMembers(run([range(0, 'unbounded')]), [
    {x: makeDate(0), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(1), y: makeDates([1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(1.5), y: makeDates([1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(2), y: makeDates([2, 3, 42, 42, 43])},
    {x: makeDate(3), y: makeDates([3, 42, 42, 43])},
    // '0' means current document and those that tie with it.
    {x: makeDate(42), y: makeDates([42, 42, 43])},
    {x: makeDate(42), y: makeDates([42, 42, 43])},
    {x: makeDate(43), y: makeDates([43])},
]);
assert.sameMembers(run([range('unbounded', 'unbounded')]), [
    {x: makeDate(0), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(1), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(1.5), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(2), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(3), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
    {x: makeDate(43), y: makeDates([0, 1, 1.5, 2, 3, 42, 42, 43])},
]);

// Unlike '0', 'current' always means the current document.
assert.sameMembers(run([range('current', 'current'), {$match: {x: makeDate(42)}}]), [
    {x: makeDate(42), y: makeDates([42])},
    {x: makeDate(42), y: makeDates([42])},
]);
assert.sameMembers(run([range('current', +1), {$match: {x: makeDate(42)}}]), [
    {x: makeDate(42), y: makeDates([42, 42, 43])},
    {x: makeDate(42), y: makeDates([42, 43])},
]);
assert.sameMembers(run([range(-40, 'current'), {$match: {x: makeDate(42)}}]), [
    {x: makeDate(42), y: makeDates([2, 3, 42])},
    {x: makeDate(42), y: makeDates([2, 3, 42, 42])},
]);

// The window can be empty even if it's unbounded on one side.
assert.sameMembers(run([range('unbounded', -40)]), [
    {x: makeDate(0), y: makeDates([])},
    {x: makeDate(1), y: makeDates([])},
    {x: makeDate(1.5), y: makeDates([])},
    {x: makeDate(2), y: makeDates([])},
    {x: makeDate(3), y: makeDates([])},
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2])},
    {x: makeDate(42), y: makeDates([0, 1, 1.5, 2])},
    {x: makeDate(43), y: makeDates([0, 1, 1.5, 2, 3])},
]);
assert.sameMembers(run([range(+40, 'unbounded')]), [
    {x: makeDate(0), y: makeDates([42, 42, 43])},
    {x: makeDate(1), y: makeDates([42, 42, 43])},
    {x: makeDate(1.5), y: makeDates([42, 42, 43])},
    {x: makeDate(2), y: makeDates([42, 42, 43])},
    {x: makeDate(3), y: makeDates([43])},
    {x: makeDate(42), y: makeDates([])},
    {x: makeDate(42), y: makeDates([])},
    {x: makeDate(43), y: makeDates([])},
]);

// Time-based windows reset between partitions.
assert.commandWorked(coll.updateMany({}, {$set: {partition: "A"}}));
assert.commandWorked(coll.insert([
    {partition: "B", x: makeDate(43)},
    {partition: "B", x: makeDate(44)},
    {partition: "B", x: makeDate(45)},
]));
assert.sameMembers(run([range(-5, 0)]), [
    {partition: "A", x: makeDate(0), y: makeDates([0])},
    {partition: "A", x: makeDate(1), y: makeDates([0, 1])},
    {partition: "A", x: makeDate(1.5), y: makeDates([0, 1, 1.5])},
    {partition: "A", x: makeDate(2), y: makeDates([0, 1, 1.5, 2])},
    {partition: "A", x: makeDate(3), y: makeDates([0, 1, 1.5, 2, 3])},
    {partition: "A", x: makeDate(42), y: makeDates([42, 42])},
    {partition: "A", x: makeDate(42), y: makeDates([42, 42])},
    {partition: "A", x: makeDate(43), y: makeDates([42, 42, 43])},

    {partition: "B", x: makeDate(43), y: makeDates([43])},
    {partition: "B", x: makeDate(44), y: makeDates([43, 44])},
    {partition: "B", x: makeDate(45), y: makeDates([43, 44, 45])},
]);
assert.commandWorked(coll.deleteMany({partition: "B"}));
assert.commandWorked(coll.updateMany({}, [{$unset: 'partition'}]));

// If the sortBy is a non-Date, we throw an error.
assert.commandWorked(coll.insert([
    {},
]));
error = assert.throws(() => {
    run([range('unbounded', 'unbounded')]);
});
assert.commandFailedWithCode(error, 5429513);

assert.commandWorked(coll.remove({x: {$exists: false}}));
assert.commandWorked(coll.insert([
    {x: 0},
]));
error = assert.throws(() => {
    run([range('unbounded', 'unbounded')]);
});
assert.commandFailedWithCode(error, 5429513);
})();
