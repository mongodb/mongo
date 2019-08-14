// SERVER-4588 Add option to $unwind to emit array index.
(function() {
"use strict";

const coll = db.server4588;
coll.drop();

assert.commandWorked(coll.insert({_id: 0}));
assert.commandWorked(coll.insert({_id: 1, x: null}));
assert.commandWorked(coll.insert({_id: 2, x: []}));
assert.commandWorked(coll.insert({_id: 3, x: [1, 2, 3]}));
assert.commandWorked(coll.insert({_id: 4, x: 5}));

// Without includeArrayIndex.
let actualResults = coll.aggregate([{$unwind: {path: "$x"}}, {$sort: {_id: 1, x: 1}}]).toArray();
let expectedResults = [
    {_id: 3, x: 1},
    {_id: 3, x: 2},
    {_id: 3, x: 3},
    {_id: 4, x: 5},
];
assert.eq(expectedResults, actualResults, "Incorrect results for normal $unwind");

// With includeArrayIndex, index inserted into a new field.
actualResults =
    coll.aggregate([{$unwind: {path: "$x", includeArrayIndex: "index"}}, {$sort: {_id: 1, x: 1}}])
        .toArray();
expectedResults = [
    {_id: 3, x: 1, index: NumberLong(0)},
    {_id: 3, x: 2, index: NumberLong(1)},
    {_id: 3, x: 3, index: NumberLong(2)},
    {_id: 4, x: 5, index: null},
];
assert.eq(expectedResults, actualResults, "Incorrect results $unwind with includeArrayIndex");

// With both includeArrayIndex and preserveNullAndEmptyArrays.
actualResults =
    coll.aggregate([
            {$unwind: {path: "$x", includeArrayIndex: "index", preserveNullAndEmptyArrays: true}},
            {$sort: {_id: 1, x: 1}}
        ])
        .toArray();
expectedResults = [
    {_id: 0, index: null},
    {_id: 1, x: null, index: null},
    {_id: 2, index: null},
    {_id: 3, x: 1, index: NumberLong(0)},
    {_id: 3, x: 2, index: NumberLong(1)},
    {_id: 3, x: 3, index: NumberLong(2)},
    {_id: 4, x: 5, index: null},
];
assert.eq(expectedResults,
          actualResults,
          "Incorrect results $unwind with includeArrayIndex and preserveNullAndEmptyArrays");
}());
