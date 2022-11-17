// Verify that bitwise agg expressions work as expected.
// @tags: [
//   requires_fcv_63,
// ]
load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
(function() {
"use strict";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, a: NumberInt(0), b: NumberInt(127)},
    {_id: 1, a: NumberInt(1), b: NumberInt(2)},
    {_id: 2, a: NumberInt(2), b: NumberInt(3)},
    {_id: 3, a: NumberInt(3), b: NumberInt(5)},
]));

assertArrayEq({
    actual: coll.aggregate([
                    {$project: {r: {$bitNot: "$a"}}},
                ])
                .toArray()
                .map(doc => doc.r),
    expected: [-1, -2, -3, -4]
});

assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $project: {
            r: {$bitNot: 12.5},
        }
    }]
}),
                             ErrorCodes.TypeMismatch);

assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    cursor: {},
    pipeline: [{
        $project: {
            r: {$bitNot: ["$a", "$b"]},
        }
    }]
}),
                             16020);  // Error for incorrect number of arguments.
}());
