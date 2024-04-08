// @tags: [requires_non_retryable_writes, requires_fastcount]

let t = db.remove_justone;
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
