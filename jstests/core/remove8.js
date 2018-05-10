// @tags: [
//   # Cannot implicitly shard accessed collections because unsupported use of sharded collection
//   # from db.eval.
//   assumes_unsharded_collection,
//   requires_eval_command,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_fastcount,
// ]

t = db.remove8;
t.drop();

N = 1000;

function fill() {
    for (var i = 0; i < N; i++) {
        t.save({x: i});
    }
}

fill();
assert.eq(N, t.count(), "A");
t.remove({});
assert.eq(0, t.count(), "B");

fill();
assert.eq(N, t.count(), "C");
db.eval(function() {
    db.remove8.remove({});
});
assert.eq(0, t.count(), "D");
