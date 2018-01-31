// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
// ]

t = db.eval4;
t.drop();

t.save({a: 1});
t.save({a: 2});
t.save({a: 3});

assert.eq(3, t.count(), "A");

function f(x) {
    db.eval4.remove({a: x});
}

f(2);
assert.eq(2, t.count(), "B");

db.eval(f, 2);
assert.eq(2, t.count(), "C");

db.eval(f, 3);
assert.eq(1, t.count(), "D");
