// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

c = db.find_and_modify_server6993;
c.drop();

c.insert({a: [1, 2]});

c.findAndModify({query: {a: 1}, update: {$set: {'a.$': 5}}});

assert.eq(5, c.findOne().a[0]);
