// @tags: [
//   # This test does not support tojson of command objects so the simulate_atlas_proxy.js override
//   # cannot deep copy the command objects correctly.
//   simulate_atlas_proxy_incompatible,
// ]

var t = db.jstests_server23626;

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: Date.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: 'date'}}).itcount());

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: Function.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: 'javascript'}}).itcount());

t.mycoll.drop();
assert.commandWorked(t.mycoll.insert({_id: 0, a: RegExp.prototype}));
assert.eq(1, t.mycoll.find({a: {$type: 'regex'}}).itcount());
