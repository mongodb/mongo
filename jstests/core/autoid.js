// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

f = db.jstests_autoid;
f.drop();

f.save({z: 1});
a = f.findOne({z: 1});
f.update({z: 1}, {z: 2});
b = f.findOne({z: 2});
assert.eq(a._id.str, b._id.str);
c = f.update({z: 2}, {z: "abcdefgabcdefgabcdefg"});
c = f.findOne({});
assert.eq(a._id.str, c._id.str);
