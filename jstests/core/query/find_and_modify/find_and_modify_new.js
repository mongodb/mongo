// @tags: [requires_fastcount]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({x: 1}));
const ret = t.findAndModify({query: {x: 1}, update: {$set: {x: 2}}, new: true});
assert.eq(2, ret.x, tojson(ret));

assert.eq(1, t.count());
