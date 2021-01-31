/**
 * Tests that projecting a non-existent subfield behaves identically in both query and aggregation.
 */
(function() {
"use strict";
const coll = db.c;
coll.drop();

assert.commandWorked(coll.insert({a: [1]}));
assert.commandWorked(coll.insert({a: {c: 1}}));
assert.commandWorked(coll.insert({a: [{c: 1}, {b: 1, c: 1}, {c: 1}]}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 1}));

assert.eq(coll.aggregate([{$project: {'a.b': 1}}, {$sort: {_id: 1}}]).toArray(),
          coll.find({}, {'a.b': 1}).sort({_id: 1}).toArray());
}());
