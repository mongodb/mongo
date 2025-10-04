// @tags: [requires_non_retryable_writes, requires_fastcount]

let t = db.remove3;
t.drop();

for (let i = 1; i <= 8; i++) {
    t.save({_id: i, x: i});
}

assert.eq(8, t.count(), "A");

t.remove({x: {$lt: 5}});
assert.eq(4, t.count(), "B");

t.remove({_id: 5});
assert.eq(3, t.count(), "C");

t.remove({_id: {$lt: 8}});
assert.eq(1, t.count(), "D");
