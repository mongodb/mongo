const t = db[jsTestName()];
t.drop();

let orig = {"": 1, _id: 2, "a": 3, "b": 4};
assert.commandWorked(t.insert(orig));
assert.commandWorked(t.update({}, {$set: {"c": 5}}));
orig["c"] = 5;
assert.docEq(orig, t.findOne(), "after $set");  // SERVER-2651
