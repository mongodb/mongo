// SERVER-8568: Adding $sqrt expression
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.sqrt;
coll.drop();
assert.commandWorked(coll.insert({_id: 0}));

// Helper for testing that op returns expResult.
function testOp(op, expResult) {
    let pipeline = [{$project: {_id: 0, result: op}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
}

// Helper for testing that op results in error with code errorCode.
function testError(op, errorCode) {
    let pipeline = [{$project: {_id: 0, result: op}}];
    assertErrorCode(coll, pipeline, errorCode);
}

// Valid input: Numeric arg >= 0, null, or NaN.

testOp({$sqrt: [100]}, 10);
testOp({$sqrt: [0]}, 0);
// All types converted to doubles.
testOp({$sqrt: [NumberLong("100")]}, 10);
// LLONG_MAX is converted to a double.
testOp({$sqrt: [NumberLong("9223372036854775807")]}, 3037000499.97605);
// Null inputs result in null.
testOp({$sqrt: [null]}, null);
// NaN inputs result in NaN.
testOp({$sqrt: [NaN]}, NaN);

// Invalid input: non-numeric/non-null, arg is negative.

// Arg must be numeric or null.
testError({$sqrt: ["string"]}, 28765);
// Args cannot be negative.
testError({$sqrt: [-1]}, 28714);
