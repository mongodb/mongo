// Cannot implicitly shard accessed collections because of following errmsg: A single
let t = db[jsTestName()];
t.drop();

assert.commandWorked(t.update({"x.y": 2}, {$inc: {a: 7}}, true));

let correct = {a: 7, x: {y: 2}};
let got = t.findOne();
delete got._id;
assert.docEq(correct, got, "A");
