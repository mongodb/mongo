// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

t = db.find_and_modify_where;
t.drop();

t.insert({_id: 1, x: 1});

res = t.findAndModify({query: {$where: "return this.x == 1"}, update: {$set: {y: 1}}});

assert.eq(1, t.findOne().y);
