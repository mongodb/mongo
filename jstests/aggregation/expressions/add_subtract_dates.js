// SERVER-6239 reenable $add and $subtract with dates with better semantics
// Note: error conditions tested also in server6240.js
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var millis = 12345;
var num = 54312;

// Clear db
const coll = db[jsTestName()];
coll.drop();

// Populate db
assert.commandWorked(coll.insertOne({date: new Date(millis), num: num}));

function test(expression, expected) {
    var res = coll.aggregate({$project: {out: expression}});
    assert.eq(res.toArray()[0].out, expected, tojson(expression));
}
function fail(expression, code) {
    assertErrorCode(coll, {$project: {out: expression}}, code);
}

test({$subtract: ['$date', '$date']}, NumberLong(0));
test({$subtract: ['$date', '$num']}, new Date(millis - num));
fail({$subtract: ['$num', '$date']}, [16556, ErrorCodes.TypeMismatch]);

fail({$add: ['$date', '$date']}, 16612);
test({$add: ['$date', '$num']}, new Date(millis + num));
test({$add: ['$num', '$date']}, new Date(millis + num));

// addition supports any number of arguments
test({$add: ['$date']}, new Date(millis));
test({$add: ['$num', '$date', '$num']}, new Date(millis + num + num));
