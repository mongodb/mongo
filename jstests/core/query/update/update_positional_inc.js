const t = db[jsTestName()];
t.drop();

let x = {_id: 1, arr: ["A1", "B1", "C1"]};
assert.commandWorked(t.insert(x));
assert.eq(x, t.findOne(), "A1");

x.arr[0] = "A2";
assert.commandWorked(t.update({arr: "A1"}, {$set: {"arr.$": "A2"}}));
assert.eq(x, t.findOne(), "A2");

assert.commandWorked(t.createIndex({arr: 1}));
x.arr[0] = "A3";
assert.commandWorked(t.update({arr: "A2"}, {$set: {"arr.$": "A3"}}));
assert.eq(x, t.findOne(), "A3");  // SERVER-1055
