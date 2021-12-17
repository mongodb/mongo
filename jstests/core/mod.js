// Tests the behavior of $mod for match expressions.

(function() {
"use strict";

const coll = db.mod;
coll.drop();

function assertDocumentsFromMod(divider, remainder, expectedDocuments) {
    const actualDocuments = coll.find({a: {$mod: [divider, remainder]}}).sort({_id: 1}).toArray();
    assert.eq(expectedDocuments, actualDocuments);
}

// Check mod with divisor and remainder which do not fit into int type.
assert.commandWorked(coll.insert([
    {_id: 1, a: 4000000000},
    {_id: 2, a: 15000000000},
    {_id: 3, a: -4000000000},
    {_id: 4, a: -15000000000},
    {_id: 5, a: 4000000000.12345},
    {_id: 6, a: 15000000000.12345},
    {_id: 7, a: -4000000000.12345},
    {_id: 8, a: -15000000000.12345},
    {_id: 9, a: NumberDecimal(4000000000.12345)},
    {_id: 10, a: NumberDecimal(15000000000.12345)},
    {_id: 11, a: NumberDecimal(-4000000000.12345)},
    {_id: 12, a: NumberDecimal(-15000000000.12345)},
]));

assertDocumentsFromMod(4000000000, 0, [
    {_id: 1, a: 4000000000},
    {_id: 3, a: -4000000000},
    {_id: 5, a: 4000000000.12345},
    {_id: 7, a: -4000000000.12345},
    {_id: 9, a: NumberDecimal(4000000000.12345)},
    {_id: 11, a: NumberDecimal(-4000000000.12345)},
]);

assertDocumentsFromMod(-4000000000, 0, [
    {_id: 1, a: 4000000000},
    {_id: 3, a: -4000000000},
    {_id: 5, a: 4000000000.12345},
    {_id: 7, a: -4000000000.12345},
    {_id: 9, a: NumberDecimal(4000000000.12345)},
    {_id: 11, a: NumberDecimal(-4000000000.12345)},
]);

assertDocumentsFromMod(10000000000, 5000000000, [
    {_id: 2, a: 15000000000},
    {_id: 6, a: 15000000000.12345},
    {_id: 10, a: NumberDecimal(15000000000.12345)},
]);

assertDocumentsFromMod(10000000000, -5000000000, [
    {_id: 4, a: -15000000000},
    {_id: 8, a: -15000000000.12345},
    {_id: 12, a: NumberDecimal(-15000000000.12345)},
]);

assertDocumentsFromMod(-10000000000, 5000000000, [
    {_id: 2, a: 15000000000},
    {_id: 6, a: 15000000000.12345},
    {_id: 10, a: NumberDecimal(15000000000.12345)},
]);

assertDocumentsFromMod(-10000000000, -5000000000, [
    {_id: 4, a: -15000000000},
    {_id: 8, a: -15000000000.12345},
    {_id: 12, a: NumberDecimal(-15000000000.12345)},
]);

assert(coll.drop());

// Check truncation of input argument for mod operator.
assert.commandWorked(coll.insert([
    {_id: 1, a: 4.2},
    {_id: 2, a: 4.5},
    {_id: 3, a: 4.7},
    {_id: 4, a: NumberDecimal(4.2)},
    {_id: 5, a: NumberDecimal(4.5)},
    {_id: 6, a: NumberDecimal(4.7)},
]));

assertDocumentsFromMod(4, 0, [
    {_id: 1, a: 4.2},
    {_id: 2, a: 4.5},
    {_id: 3, a: 4.7},
    {_id: 4, a: NumberDecimal(4.2)},
    {_id: 5, a: NumberDecimal(4.5)},
    {_id: 6, a: NumberDecimal(4.7)},
]);

assert(coll.drop());

// Check more basic mod usage.
assert.commandWorked(coll.insert([
    {_id: 1, str: "abc123", a: 0},
    {_id: 2, str: "xyz123", a: 5},
    {_id: 3, str: "ijk123", a: 12},
    {_id: 4, s: "array", a: [0, 7]},
    {_id: 5, s: "array", a: [1, 7]},
    {_id: 6, s: "array", a: [-5]},
]));

assert.eq(1, coll.find({a: {$mod: [-10, -5]}}).itcount());
assert.eq(1, coll.find({a: {$mod: [4, -1]}}).itcount());
assert.eq(4, coll.find({a: {$mod: [5, 0]}}).itcount());
assert.eq(3, coll.find({a: {$mod: [12, 0]}}).itcount());
assert.eq(1, coll.find({a: {$mod: [12, 1]}}).itcount());

assert.commandWorked(coll.insert([
    {_id: 7, arr: [1, [2, 3], [[4]]]},
    {_id: 8, arr: [{b: [1, 2, 3]}, {b: [5]}]},
    {_id: 9, arr: [{b: [-1]}, {b: [5]}]},
    {_id: 10, arr: "string"},
]));

// Check nested arrays and dotted paths.
assert.eq(1, coll.find({arr: {$mod: [1, 0]}}).itcount());
assert.eq(0, coll.find({arr: {$mod: [4, 0]}}).itcount());
assert.eq(1, coll.find({"arr.b": {$mod: [3, 0]}}).itcount());
assert.eq(2, coll.find({"arr.b": {$mod: [5, 0]}}).itcount());
assert.eq(0, coll.find({"arr.b.c": {$mod: [5, 0]}}).itcount());

// Check with different data types.
assert.eq(6, coll.find({a: {$mod: [1, NumberLong(0)]}}).itcount());
assert.eq(3, coll.find({a: {$mod: [NumberLong(5), NumberDecimal(2.1)]}}).itcount());
assert.eq(3, coll.find({a: {$mod: [NumberDecimal(5.001), NumberDecimal(2.1)]}}).itcount());
assert.eq(1, coll.find({a: {$mod: [NumberInt(7), NumberInt(1)]}}).itcount());

// Check on fields that are not numbers or do not exist.
assert.eq(0, coll.find({str: {$mod: [10, 1]}}).itcount());
assert.eq(0, coll.find({s: {$mod: [10, 1]}}).itcount());
assert.eq(0, coll.find({noField: {$mod: [10, 1]}}).itcount());

// Check divide by zero.
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [0, 1]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [0, 0]}}}),
                             ErrorCodes.BadValue);

// Check failures with different data types.
assert.commandFailedWithCode(
    db.runCommand(
        {find: coll.getName(), filter: {a: {$mod: [NumberDecimal(0.001), NumberDecimal(1.001)]}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    db.runCommand(
        {find: coll.getName(), filter: {a: {$mod: [NumberInt(1), NumberInt(0), NumberInt(0)]}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), filter: {a: {$mod: [NumberDecimal(0.1)]}}}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    db.runCommand(
        {find: coll.getName(), filter: {a: {$mod: [NumberDecimal(0), NumberDecimal(0)]}}}),
    ErrorCodes.BadValue);

// Check incorrect arity.
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [10, 1, 1]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [10]}}}),
                             ErrorCodes.BadValue);

// Check remainder, divisor not a number.
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: ["a", 0]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: ["a", "b"]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [3, "r"]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [10, null]}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), filter: {a: {$mod: [null, 2]}}}),
                             ErrorCodes.BadValue);
}());
