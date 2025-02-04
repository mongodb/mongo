// Test the use of $each in $addToSet

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({_id: 1}));

assert.commandWorked(t.update({_id: 1}, {$addToSet: {a: {$each: [6, 5, 4]}}}));
assert.eq(t.findOne(), {_id: 1, a: [6, 5, 4]}, "A1");

assert.commandWorked(t.update({_id: 1}, {$addToSet: {a: {$each: [3, 2, 1]}}}));
assert.eq(t.findOne(), {_id: 1, a: [6, 5, 4, 3, 2, 1]}, "A2");

assert.commandWorked(t.update({_id: 1}, {$addToSet: {a: {$each: [4, 7, 9, 2]}}}));
assert.eq(t.findOne(), {_id: 1, a: [6, 5, 4, 3, 2, 1, 7, 9]}, "A3");

assert.commandWorked(t.update({_id: 1}, {$addToSet: {a: {$each: [12, 13, 12]}}}));
assert.eq(t.findOne(), {_id: 1, a: [6, 5, 4, 3, 2, 1, 7, 9, 12, 13]}, "A4");

assert.writeError(t.update({_id: 1}, {$addToSet: {a: {$each: 0}}}));
assert.writeError(t.update({_id: 1}, {$addToSet: {a: {$each: {a: 1}}}}));
