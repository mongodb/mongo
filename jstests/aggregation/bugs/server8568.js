// SERVER-8568: Adding $sqrt expression

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');
load("jstests/libs/sbe_assert_error_override.js");         // Override error-code-checking APIs.
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";  // TODO SERVER-78596: remove this import

(function() {
'use strict';
var coll = db.sqrt;
coll.drop();
assert.commandWorked(coll.insert({_id: 0}));

// Helper for testing that op returns expResult.
function testOp(op, expResult) {
    var pipeline = [{$project: {_id: 0, result: op}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
}

// Helper for testing that op results in error with code errorCode.
function testError(op, errorCode) {
    var pipeline = [{$project: {_id: 0, result: op}}];
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
// TODO SERVER-78596: Remove 'featureFlagSbeFull' check when fixed.
if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    // NaN inputs result in NaN.
    testOp({$sqrt: [NaN]}, NaN);
}

// Invalid input: non-numeric/non-null, arg is negative.

// Arg must be numeric or null.
testError({$sqrt: ["string"]}, 28765);
// Args cannot be negative.
testError({$sqrt: [-1]}, 28714);
}());
