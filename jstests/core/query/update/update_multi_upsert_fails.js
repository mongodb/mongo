// @tags: [requires_non_retryable_writes]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.update({_id: 1}, {_id: 1, x: 1, y: 2}, true, false));
assert(t.findOne({_id: 1}), "A");

assert.writeError(t.update({_id: 2}, {_id: 2, x: 1, y: 2}, true, true));
