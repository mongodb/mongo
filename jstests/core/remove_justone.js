// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection, requires_non_retryable_writes, requires_fastcount]

t = db.remove_justone;
t.drop();

t.insert({x: 1});
t.insert({x: 1});
t.insert({x: 1});
t.insert({x: 1});

assert.eq(4, t.count());

t.remove({x: 1}, true);
assert.eq(3, t.count());

t.remove({x: 1});
assert.eq(0, t.count());
