/**
 * Test the $size expression.
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.expression_size;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, arr: []}));
assert.commandWorked(coll.insert({_id: 1, arr: [1]}));
assert.commandWorked(coll.insert({_id: 2, arr: ["asdf", "asdfasdf"]}));
assert.commandWorked(coll.insert({_id: 3, arr: [1, "asdf", 1234, 4.3, {key: 23}]}));
assert.commandWorked(coll.insert({_id: 4, arr: [3, [31, 31, 13, 13]]}));

const result = coll.aggregate([{$sort: {_id: 1}}, {$project: {_id: 0, length: {$size: "$arr"}}}]);
assert.eq(result.toArray(), [{length: 0}, {length: 1}, {length: 2}, {length: 5}, {length: 2}]);

// $size raises this code in classic; SBE raises its own code for the same condition.
const kSizeNotArrayCodes = [17124, 8069800];

// $size errors on any non-array input, including null and a missing field.
for (const nonArrayValue of [231, "asdf", 4.3, true, {key: 23}, null]) {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, arr: nonArrayValue}));
    assertErrorCode(coll, {$project: {_id: 0, length: {$size: "$arr"}}}, kSizeNotArrayCodes);
}

coll.drop();
assert.commandWorked(coll.insert({_id: 0}));
assertErrorCode(coll, {$project: {_id: 0, length: {$size: "$arr"}}}, kSizeNotArrayCodes);

// The result should be a 32-bit int for small arrays.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, arr: [1, 2, 3]}));
const typeResult = coll
    .aggregate([{$project: {_id: 0, lengthType: {$type: {$size: "$arr"}}}}])
    .toArray();
assert.eq(typeResult, [{lengthType: "int"}]);
