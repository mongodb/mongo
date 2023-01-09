// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [assumes_unsharded_collection]

t = db.find_and_modify_server7660;
t.drop();

a = t.findAndModify(
    {query: {foo: 'bar'}, update: {$set: {bob: 'john'}}, sort: {foo: 1}, upsert: true, new: true});

b = t.findOne();
assert.eq(a, b);
assert.eq("bar", a.foo);
assert.eq("john", a.bob);
