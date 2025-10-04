// @tags: [requires_non_retryable_writes, requires_fastcount]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({x: 1}));
assert.commandWorked(t.insert({x: 1}));
assert.commandWorked(t.insert({x: 1}));
assert.commandWorked(t.insert({x: 1}));

assert.eq(4, t.count());

assert.commandWorked(t.remove({x: 1}, true));
assert.eq(3, t.count());

assert.commandWorked(t.remove({x: 1}));
assert.eq(0, t.count());
