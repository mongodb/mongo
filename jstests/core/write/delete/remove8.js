// @tags: [
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
// ]

let t = db.remove8;
t.drop();

let N = 1000;

function fill() {
    for (var i = 0; i < N; i++) {
        t.save({x: i});
    }
}

fill();
assert.eq(N, t.find().itcount(), "A");
t.remove({});
assert.eq(0, t.find().itcount(), "B");

fill();
assert.eq(N, t.find().itcount(), "C");
t.remove({});
assert.eq(0, t.find().itcount(), "D");
