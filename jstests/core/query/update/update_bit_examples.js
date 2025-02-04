// @tags: [
//   requires_non_retryable_writes,
// ]

// Basic examples for $bit
let res;
const coll = db[jsTestName()];
coll.drop();

// $bit and
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: NumberInt(2)}));
assert.commandWorked(coll.update({}, {$bit: {a: {and: NumberInt(4)}}}));
assert.eq(coll.findOne().a, 0);

// $bit or
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: NumberInt(2)}));
assert.commandWorked(coll.update({}, {$bit: {a: {or: NumberInt(4)}}}));
assert.eq(coll.findOne().a, 6);

// $bit xor
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.save({_id: 1, a: NumberInt(0)}));
assert.commandWorked(coll.update({}, {$bit: {a: {xor: NumberInt(4)}}}));
assert.eq(coll.findOne().a, 4);

// SERVER-19706 Empty bit operation.
assert.writeError(coll.update({}, {$bit: {a: {}}}));

// Make sure $bit on index arrays 9 and 10 when padding is needed works.
assert.commandWorked(coll.insert({_id: 2, a: [0]}));
assert.commandWorked(
    coll.update({_id: 2}, {$bit: {"a.9": {or: NumberInt(0)}, "a.10": {or: NumberInt(0)}}}));
res = coll.find({_id: 2}).toArray();
assert.eq(res[0]["a"], [0, null, null, null, null, null, null, null, null, 0, 0]);
