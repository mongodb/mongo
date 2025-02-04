// @tags: [requires_non_retryable_writes]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insertMany([{a: 1, b: 1}, {a: 2, b: 1}, {a: 3, b: 1}]));

assert.eq(3, t.find().length());
assert.commandWorked(t.remove({b: 1}));
assert.eq(0, t.find().length());
