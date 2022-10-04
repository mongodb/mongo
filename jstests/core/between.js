/**
 * Tests the $between operator in simple form in the match language on unencrypted data.
 * @tags: [ requires_fcv_62, multiversion_incompatible ]
 */

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.
load('jstests/libs/analyze_plan.js');         // For getWinningPlan.

const coll = db.between_coll;
coll.drop();

// populate the collection with various values.
let i;
for (i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({v: i, _id: i}));
}
assert.commandWorked(coll.insert({v: NumberLong(4), _id: i++}));
assert.commandWorked(coll.insert({v: NumberDecimal(9.2855), _id: i++}));

let stringVals = ["hello", "world", "galaxy", "apple", "apartment"];
stringVals.forEach(string => {
    assert.commandWorked(coll.insert({v: string, _id: i++}));
});

assert.commandWorked(coll.insert({v: [2, 4, 6], _id: i++}));
assert.commandWorked(coll.insert({v: [2, "galaxy", [8]], _id: i++}));
assert.commandWorked(coll.insert({v: ISODate("1998-08-03T20:34:00.000Z"), _id: i++}));
assert.commandWorked(coll.insert({v: ISODate("2022-01-01T00:00:00.000Z"), _id: i++}));

//  $between given an array of numbers.
let expected = [{v: 3}, {v: 4}, {v: NumberLong(4)}, {v: 5}, {v: 6}, {v: 7}, {v: 8}, {v: [2, 4, 6]}];
assertArrayEq(
    {actual: coll.find({v: {$between: [3, 8]}}).toArray(), expected, fieldsToSkip: ["_id"]});

// $between given an array of dates.
expected = [{v: ISODate("1998-08-03T20:34:00.000Z")}];
assertArrayEq({
    actual:
        coll.find({
                v: {
                    $between:
                        [ISODate("1996-02-03T20:34:00.000Z"), ISODate("2000-04-03T20:34:00.000Z")]
                }
            })
            .toArray(),
    expected,
    fieldsToSkip: ["_id"]
});

//  $between nested inide a $not expression.
expected = [
    {v: 0},
    {v: 1},
    {v: 2},
    {v: 9},
    {v: NumberDecimal(9.2855)},
    {v: "hello"},
    {v: "world"},
    {v: "galaxy"},
    {v: "apple"},
    {v: "apartment"},
    {v: [2, "galaxy", [8]]},
    {v: ISODate("1998-08-03T20:34:00.000Z")},
    {v: ISODate("2022-01-01T00:00:00.000Z")}
];
assertArrayEq({
    actual: coll.find({v: {$not: {$between: [3, 8]}}}).toArray(),
    expected,
    fieldsToSkip: ["_id"]
});

// $between inline with other expressions.
expected = [{v: 0}, {v: "galaxy"}, {v: "apple"}, {v: "apartment"}, {v: [2, "galaxy", [8]]}];
assertArrayEq({
    actual: coll.find({$or: [{v: 0}, {v: {$between: ['a', 'gb']}}]}).toArray(),
    expected,
    fieldsToSkip: ["_id"]
});

// $between returns empty result.
expected = [];
assertArrayEq({actual: coll.find({v: {$between: [NumberInt(32), {}]}}).toArray(), expected});

// $between uses an index scan with arrays.
coll.createIndex({v: 1});
let betweenExplainArray = coll.find({v: {$between: [2, 5]}}).explain().queryPlanner;
let winningPlan = getWinningPlan(betweenExplainArray);
assert(isIxscan(db, winningPlan));

// $between uses an index scan with the proper bounds without arrays.
coll.dropIndex({v: 1});
assert.commandWorked(coll.deleteOne({v: [2, "galaxy", [8]], _id: 18}));
assert.commandWorked(coll.deleteOne({v: [2, 4, 6], _id: 17}));
coll.createIndex({v: 1});

winningPlan = getWinningPlan(coll.find({v: {$between: [2, 5]}}).explain().queryPlanner);
let stages = getPlanStages(winningPlan, "IXSCAN");
assert(isIxscan(db, winningPlan));
stages.forEach(stage => {
    assert.eq({v: ["[2.0, 5.0]"]}, stage.indexBounds);
});

// $between must fail if the array does not have 2 elements in it.
assert.throwsWithCode(() => {
    coll.find({v: {$between: [1]}}).toArray();
}, ErrorCodes.FailedToParse);

// $between must fail if the array does not have 2 elements in it.
assert.throwsWithCode(() => {
    coll.find({v: {$between: [1, 2, 4]}}).toArray();
}, ErrorCodes.FailedToParse);

// $between must fail if the input is not binData or an array.
assert.throwsWithCode(() => {
    coll.find({v: {$between: "apartment"}}).toArray();
}, ErrorCodes.BadValue);

// $between must fail if the input is not binData or an array.
assert.throwsWithCode(() => {
    coll.find({v: {$between: NumberDecimal(100.32)}}).toArray();
}, ErrorCodes.BadValue);
})();
