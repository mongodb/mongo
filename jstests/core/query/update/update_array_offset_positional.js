/**
 * Tests that array offset matches are not used to provide values for the positional operator.
 */
const coll = db[jsTestName()];
coll.drop();

//
// If there is no implicit array traversal, the positional operator cannot be used.
//

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [0]}));
assert.writeError(coll.update({_id: 0, "a.0": 0}, {$set: {"a.$": 1}}));

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [{b: 0}]}));
assert.writeError(coll.update({_id: 0, "a.0.b": 0}, {$set: {"a.$.b": 1}}));

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [[0]]}));
assert.writeError(coll.update({_id: 0, "a.0.0": 0}, {$set: {"a.$.0": 1}}));

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [{b: [0]}]}));
assert.writeError(coll.update({_id: 0, "a.0.b.0": 0}, {$set: {"a.$.b.0": 1}}));

//
// Array offset matches are not used to provide values for the positional operator on the same
// path.
//

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [{b: [0, 1]}]}));
assert.commandWorked(coll.update({_id: 0, "a.0.b": 1}, {$set: {"a.0.b.$": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: [0, 2]}]});

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [{b: [0, 1]}]}));
assert.commandWorked(coll.update({_id: 0, "a.b.1": 1}, {$set: {"a.$.b.1": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [{b: [0, 2]}]});

//
// Array offset matches are not used to provide values for the positional operator on a
// different path.
//

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [0]}));
assert.commandWorked(coll.update({_id: 0, a: 1, "b.0": 0}, {$set: {"a.$": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 2], b: [0]});

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [{c: 0}]}));
assert.commandWorked(coll.update({_id: 0, a: 1, "b.0.c": 0}, {$set: {"a.$": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 2], b: [{c: 0}]});

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [[0]]}));
assert.commandWorked(coll.update({_id: 0, a: 1, "b.0.0": 0}, {$set: {"a.$": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 2], b: [[0]]});

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [{c: [0]}]}));
assert.commandWorked(coll.update({_id: 0, a: 1, "b.0.c.0": 0}, {$set: {"a.$": 2}}));
assert.eq(coll.findOne({_id: 0}), {_id: 0, a: [0, 2], b: [{c: [0]}]});