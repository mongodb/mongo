/**
 * Tests that modding the smallest representable integer values by -1 does not result in integer
 * overflow. Exercises the fix for SERVER-43699.
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.test;
testColl.drop();

// Insert two documents, one with a value of -2^63 and the other with a value of -2^31.
const insertedDocs =
    [{_id: 0, val: NumberLong("-9223372036854775808")}, {_id: 1, val: NumberInt("-2147483648")}];
assert.commandWorked(testColl.insert(insertedDocs));

// For each possible integral representation of -1, confirm that overflow does not occur.
for (let divisor of [-1.0, NumberInt("-1"), NumberLong("-1"), NumberDecimal("-1")]) {
    assert.docEq(insertedDocs, testColl.find({val: {$mod: [divisor, 0]}}).sort({_id: 1}).toArray());
    assert.docEq(
        insertedDocs,
        testColl
            .aggregate(
                [{$match: {$expr: {$eq: [0, {$mod: ["$val", divisor]}]}}}, {$sort: {_id: 1}}])
            .toArray());

    // Confirm that overflow does not occur during agg expression evaluation. Also confirm that the
    // correct type is returned for each combination of input types.
    const expectedResults = [
        Object.merge(
            insertedDocs[0],
            {modVal: (divisor instanceof NumberDecimal ? NumberDecimal("-0") : NumberLong("0"))}),
        Object.merge(insertedDocs[1], {
            modVal: (divisor instanceof NumberLong          ? NumberLong("0")
                         : divisor instanceof NumberDecimal ? NumberDecimal("-0")
                                                            : 0)
        })
    ];
    assert.docEq(
        expectedResults,
        testColl
            .aggregate([{$project: {val: 1, modVal: {$mod: ["$val", divisor]}}}, {$sort: {_id: 1}}])
            .toArray());
}
})();
