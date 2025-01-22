// SERVER-10530 Would error if large objects are in first batch

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({big: Array(1024 * 1024).toString()}));
assert.commandWorked(t.insert({big: Array(16 * 1024 * 1024 - 1024).toString()}));
assert.commandWorked(t.insert({big: Array(1024 * 1024).toString()}));

assert.eq(t.aggregate().itcount(), 3);

// clean up large collection
assert(t.drop());
