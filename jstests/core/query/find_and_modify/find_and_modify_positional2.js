const t = db[jsTestName()];

const initial = {
    _id: 1,
    a: [{b: 1}],
    z: 1
};
const up = {
    "$set": {"a.$.b": 2}
};
const q = {
    _id: 1,
    "a.b": 1
};
const correct = {
    _id: 1,
    a: [{b: 2}],
    z: 1
};

t.drop();
assert.commandWorked(t.insert(initial));
assert.commandWorked(t.update(q, up));
assert.eq(correct, t.findOne());

assert(t.drop());
assert.commandWorked(t.insert(initial));
let x = t.findAndModify({query: q, update: up});
assert.eq(correct, t.findOne());

assert(t.drop());
assert.commandWorked(t.insert(initial));
x = t.findAndModify({query: {z: 1, "a.b": 1}, update: up});
assert.eq(correct, t.findOne());
