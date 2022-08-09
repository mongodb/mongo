// In SERVER-63012, translation of $add expression into sbe now defaults the translation of $add
// with no operands to a zero integer constant.
(function() {
"use strict";

const coll = db.add_empty;
coll.drop();

assert.commandWorked(coll.insert({x: 1}));
let result = coll.aggregate([{$project: {y: {$add: []}}}]).toArray();
assert.eq(result[0]["y"], 0);
}());
