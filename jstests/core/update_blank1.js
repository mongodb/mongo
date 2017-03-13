// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

t = db.update_blank1;
t.drop();

orig = {
    "": 1,
    _id: 2, "a": 3, "b": 4
};
t.insert(orig);
var res = t.update({}, {$set: {"c": 5}});
print(res);
orig["c"] = 5;
assert.docEq(orig, t.findOne(), "after $set");  // SERVER-2651
