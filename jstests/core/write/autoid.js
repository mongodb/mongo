const f = db[jsTestName()];
f.drop();

assert.commandWorked(f.insertOne({z: 1}));
let a = f.findOne({z: 1});
assert.commandWorked(f.update({z: 1}, {z: 2}));
let b = f.findOne({z: 2});
assert.eq(a._id.str, b._id.str);
assert.commandWorked(f.update({z: 2}, {z: "abcdefgabcdefgabcdefg"}));
let c = f.findOne({});
assert.eq(a._id.str, c._id.str);
