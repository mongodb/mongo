// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

t = db.server1470;
t.drop();

q = {
    "name": "first",
    "pic": {"$ref": "foo", "$id": ObjectId("4c48d04cd33a5a92628c9af6")}
};
t.update(q, {$set: {x: 1}}, true, true);
ref = t.findOne().pic;
assert.eq("object", typeof(ref));
assert.eq(q.pic["$ref"], ref["$ref"]);
assert.eq(q.pic["$id"], ref["$id"]);

// just make we haven't broken other update operators
t.drop();
t.update({_id: 1, x: {$gt: 5}}, {$set: {y: 1}}, true);
assert.eq({_id: 1, y: 1}, t.findOne());
