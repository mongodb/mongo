// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

let t = db.updateb;
t.drop();

t.update({"x.y": 2}, {$inc: {a: 7}}, true);

let correct = {a: 7, x: {y: 2}};
let got = t.findOne();
delete got._id;
assert.docEq(correct, got, "A");
