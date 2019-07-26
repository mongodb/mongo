/**
 * Test the $size expression.
 */
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");

const coll = db.expression_size;
coll.drop();

assert.writeOK(coll.insert({_id: 0, arr: []}));
assert.writeOK(coll.insert({_id: 1, arr: [1]}));
assert.writeOK(coll.insert({_id: 2, arr: ["asdf", "asdfasdf"]}));
assert.writeOK(coll.insert({_id: 3, arr: [1, "asdf", 1234, 4.3, {key: 23}]}));
assert.writeOK(coll.insert({_id: 4, arr: [3, [31, 31, 13, 13]]}));

const result = coll.aggregate([{$sort: {_id: 1}}, {$project: {_id: 0, length: {$size: "$arr"}}}]);
assert.eq(result.toArray(), [{length: 0}, {length: 1}, {length: 2}, {length: 5}, {length: 2}]);

assert.writeOK(coll.insert({arr: 231}));
assertErrorCode(coll, {$project: {_id: 0, length: {$size: "$arr"}}}, 17124);
}());
