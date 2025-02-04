
const t = db[jsTestName()];
t.drop();

const o = {
    _id: 1
};
assert.commandWorked(t.insert(o));

assert.commandWorked(t.update({}, {$addToSet: {'kids': {'name': 'Bob', 'age': '4'}}}));
let res = t.findOne();
assert.eq(res['kids'].length, 1, res);
assert.commandWorked(t.update({}, {$addToSet: {'kids': {'name': 'Dan', 'age': '2'}}}));
res = t.findOne();
assert.eq(res['kids'].length, 2, res);
