const t = db[jsTestName()];
t.drop();

let o = {_id: Math.random(), items: [null, null, null, null]};

assert.commandWorked(t.insert(o));
assert.docEq(o, t.findOne(), "A1");

o.items[0] = {
    amount: 9000,
    itemId: 1,
};
assert.commandWorked(t.update({}, {$set: {"items.0": o.items[0]}}));
assert.docEq(o, t.findOne(), "A2");

o.items[0].amount += 1000;
o.items[1] = {
    amount: 1,
    itemId: 2,
};
assert.commandWorked(t.update({}, {$inc: {"items.0.amount": 1000}, $set: {"items.1": o.items[1]}}));
assert.docEq(o, t.findOne(), "A3");
