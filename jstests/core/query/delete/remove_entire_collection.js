// @tags: [
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

const t = db[jsTestName()];
t.drop();

const N = 1000;

function fill() {
    for (var i = 0; i < N; i++) {
        assert.commandWorked(t.save({x: i}));
    }
}

fill();
assert.eq(N, t.find().itcount(), "A");
assert.commandWorked(t.remove({}));
assert.eq(0, t.find().itcount(), "B");

fill();
assert.eq(N, t.find().itcount(), "C");
assert.commandWorked(t.remove({}));
assert.eq(0, t.find().itcount(), "D");
