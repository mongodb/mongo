// Update with mods corner cases.

const f = db[jsTestName()];

f.drop();
assert.commandWorked(f.save({a: 1}));
assert.commandWorked(f.update({}, {$inc: {a: 1}}));
assert.eq(2, f.findOne().a, "A");

assert(f.drop());
assert.commandWorked(f.save({a: {b: 1}}));
assert.commandWorked(f.update({}, {$inc: {"a.b": 1}}));
assert.eq(2, f.findOne().a.b, "B");

assert(f.drop());
assert.commandWorked(f.save({a: {b: 1}}));
assert.commandWorked(f.update({}, {$set: {"a.b": 5}}));
assert.eq(5, f.findOne().a.b, "C");

assert(f.drop());
assert.commandWorked(f.save({'_id': 0}));
assert.commandFailedWithCode(f.update({}, {$set: {'_id': 5}}), ErrorCodes.ImmutableField);
assert.eq(0, f.findOne()._id, "D");

assert(f.drop());
assert.commandWorked(f.save({_id: 1, a: 1}));
assert.commandWorked(f.update({}, {$unset: {"a": 1, "b.c": 1}}));
assert.docEq({_id: 1}, f.findOne(), "E");
