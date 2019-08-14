// Basic examples for $min/$max
(function() {
"use strict";

let res;
const coll = db.update_min_max;
coll.drop();

// $min for number
coll.insert({_id: 1, a: 2});
res = coll.update({_id: 1}, {$min: {a: 1}});
assert.commandWorked(res);
assert.eq(coll.findOne({_id: 1}).a, 1);

// $max for number
coll.insert({_id: 2, a: 2});
res = coll.update({_id: 2}, {$max: {a: 1}});
assert.commandWorked(res);
assert.eq(coll.findOne({_id: 2}).a, 2);

// $min for Date
let date = new Date();
coll.insert({_id: 3, a: date});
// setMilliseconds() will roll over to change seconds if necessary.
date.setMilliseconds(date.getMilliseconds() + 2);
// Test that we have advanced the date and it's no longer the same as the one we inserted.
assert.eq(null, coll.findOne({_id: 3, a: date}));
const origDoc = coll.findOne({_id: 3});
assert.commandWorked(coll.update({_id: 3}, {$min: {a: date}}));
assert.eq(coll.findOne({_id: 3}).a, origDoc.a);

// $max for Date
coll.insert({_id: 4, a: date});
// setMilliseconds() will roll over to change seconds if necessary.
date.setMilliseconds(date.getMilliseconds() + 2);
// Test that we have advanced the date and it's no longer the same as the one we inserted.
assert.eq(null, coll.findOne({_id: 4, a: date}));
res = coll.update({_id: 4}, {$max: {a: date}});
assert.commandWorked(res);
assert.eq(coll.findOne({_id: 4}).a, date);

// $max for small number
coll.insert({_id: 5, a: 1e-15});
// Slightly bigger than 1e-15.
const biggerval = 0.000000000000001000000000000001;
res = coll.update({_id: 5}, {$max: {a: biggerval}});
assert.commandWorked(res);
assert.eq(coll.findOne({_id: 5}).a, biggerval);

// $min for a small number
coll.insert({_id: 6, a: biggerval});
res = coll.update({_id: 6}, {$min: {a: 1e-15}});
assert.commandWorked(res);
assert.eq(coll.findOne({_id: 6}).a, 1e-15);

// $max with positional operator
let insertdoc = {_id: 7, y: [{a: 2}, {a: 6}, {a: [9, 1, 1]}]};
coll.insert(insertdoc);
res = coll.update({_id: 7, "y.a": 6}, {$max: {"y.$.a": 7}});
assert.commandWorked(res);
insertdoc.y[1].a = 7;
assert.docEq(coll.findOne({_id: 7}), insertdoc);

// $min with positional operator
insertdoc = {
    _id: 8,
    y: [{a: 2}, {a: 6}, {a: [9, 1, 1]}]
};
coll.insert(insertdoc);
res = coll.update({_id: 8, "y.a": 6}, {$min: {"y.$.a": 5}});
assert.commandWorked(res);
insertdoc.y[1].a = 5;
assert.docEq(coll.findOne({_id: 8}), insertdoc);
}());
