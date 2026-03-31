let t = db[jsTestName()];

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: Date.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: "date"}}).itcount());

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: Function.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: "javascript"}}).itcount());

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: RegExp.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: "regex"}}).itcount());
