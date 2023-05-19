// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

let t = db.find_and_modify_sever6588;

let initial = {_id: 1, a: [{b: 1}], z: 1};
let up = {"$set": {"a.$.b": 2}};
let q = {_id: 1, "a.b": 1};
let correct = {_id: 1, a: [{b: 2}], z: 1};

t.drop();
t.insert(initial);
t.update(q, up);
assert.eq(correct, t.findOne());

t.drop();
t.insert(initial);
let x = t.findAndModify({query: q, update: up});
assert.eq(correct, t.findOne());

t.drop();
t.insert(initial);
x = t.findAndModify({query: {z: 1, "a.b": 1}, update: up});
assert.eq(correct, t.findOne());
