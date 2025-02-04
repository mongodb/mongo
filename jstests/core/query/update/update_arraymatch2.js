// @tags: [requires_multi_updates, requires_non_retryable_writes]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({}));
assert.commandWorked(t.insert({x: [1, 2, 3]}));
assert.commandWorked(t.insert({x: 99}));
assert.commandWorked(t.update({x: 2}, {$inc: {"x.$": 1}}, false, true));
assert(t.findOne({x: 1}).x[1] == 3, "A1");

assert.commandWorked(t.insert({x: {y: [8, 7, 6]}}));
assert.commandWorked(t.update({'x.y': 7}, {$inc: {"x.y.$": 1}}, false, true));
assert.eq(8, t.findOne({"x.y": 8}).x.y[1], "B1");

assert.commandWorked(t.insert({x: [90, 91, 92], y: ['a', 'b', 'c']}));
assert.commandWorked(t.update({x: 92}, {$set: {'y.$': 'z'}}, false, true));
assert.eq('z', t.findOne({x: 92}).y[2], "B2");
