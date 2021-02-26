// SERVER-23029 added a new expression, $reverseArray, which consumes an array or a nullish value
// and produces either the reversed version of that array, or null. In this test file, we check the
// behavior and error cases.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.
load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.

(function() {
"use strict";

var coll = db.reverseArray;
coll.drop();

// We need a document to flow through the pipeline, even though we don't care what fields it
// has.
coll.insert({
    nullField: null,
    undefField: undefined,
    embedded: [[1, 2], [3, 4]],
    singleElem: [1],
    normal: [1, 2, 3],
    num: 1,
    empty: []
});

assertErrorCode(coll, [{$project: {reversed: {$reverseArray: 1}}}], 34435);
assertErrorCode(coll, [{$project: {reversed: {$reverseArray: "$num"}}}], 34435);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [1, 2]}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [2, 1]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [[1, 2]]}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[1, 2]]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$notAField"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [[1, 2], [3, 4]]}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[3, 4], [1, 2]]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$embedded"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[3, 4], [1, 2]]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: null}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$nullField"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: undefined}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$undefField"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [1]}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [1]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$singleElem"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [1]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$normal"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [3, 2, 1]);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: []}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, []);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$empty"}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, []);
}());
