
t = db.find_and_modify_sever6588;

initial = {
    _id: 1,
    a: [{b: 1}],
    z: 1
};
up = {
    "$set": {"a.$.b": 2}
};
q = {
    _id: 1,
    "a.b": 1
};
correct = {
    _id: 1,
    a: [{b: 2}],
    z: 1
};

t.drop();
t.insert(initial);
t.update(q, up);
assert.eq(correct, t.findOne());

t.drop();
t.insert(initial);
x = t.findAndModify({query: q, update: up});
assert.eq(correct, t.findOne());

t.drop();
t.insert(initial);
x = t.findAndModify({query: {z: 1, "a.b": 1}, update: up});
assert.eq(correct, t.findOne());
