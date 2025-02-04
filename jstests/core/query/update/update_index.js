const f = db[jsTestName()];

f.drop();
assert.commandWorked(f.save({a: 4}));
assert.commandWorked(f.update({a: 4}, {$inc: {a: 2}}));
assert.eq(6, f.findOne().a);

assert(f.drop());
assert.commandWorked(f.save({a: 4}));
assert.commandWorked(f.createIndex({a: 1}));
assert.commandWorked(f.update({a: 4}, {$inc: {a: 2}}));
assert.eq(6, f.findOne().a);

// Verify that drop clears the index
assert(f.drop());
assert.commandWorked(f.save({a: 4}));
assert.commandWorked(f.update({a: 4}, {$inc: {a: 2}}));
assert.eq(6, f.findOne().a);
