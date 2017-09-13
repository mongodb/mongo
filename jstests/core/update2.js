// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

f = db.ed_db_update2;

f.drop();
f.save({a: 4});
f.update({a: 4}, {$inc: {a: 2}});
assert.eq(6, f.findOne().a);

f.drop();
f.save({a: 4});
f.ensureIndex({a: 1});
f.update({a: 4}, {$inc: {a: 2}});
assert.eq(6, f.findOne().a);

// Verify that drop clears the index
f.drop();
f.save({a: 4});
f.update({a: 4}, {$inc: {a: 2}});
assert.eq(6, f.findOne().a);
