// Tests for $add, $subtract and $multiply aggregation expression type promotion on overflow
// @tags: [require_fcv_71]
(function() {
"use strict";

const coll = db.arith_overflow;

function runTest(operator, expectedResults) {
    const result =
        coll.aggregate([{$project: {res: {[operator]: ["$lhs", "$rhs"]}}}, {$sort: {_id: 1}}])
            .toArray()
            .map(r => r.res);
    assert.eq(result, expectedResults);
}

// $add
coll.drop();
assert.commandWorked(coll.insert({_id: 0, lhs: NumberInt(2e+9), rhs: NumberInt(2e+9)}));
assert.commandWorked(coll.insert({_id: 1, lhs: NumberLong(9e+18), rhs: NumberLong(9e+18)}));

runTest("$add", [NumberLong(4e+9), 1.8e+19]);

// $subtract
coll.drop();
assert.commandWorked(coll.insert({_id: 0, lhs: NumberInt(2e+9), rhs: NumberInt(-2e+9)}));
assert.commandWorked(coll.insert({_id: 1, lhs: NumberLong(9e+18), rhs: NumberLong(-9e+18)}));

runTest("$subtract", [NumberLong(4e+9), 1.8e+19]);
// $multiply uses same arguments
runTest("$multiply", [NumberLong(-4e+18), -8.1e+37]);
}());
