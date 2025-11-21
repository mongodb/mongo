// @tags: [
//   requires_non_retryable_writes,
//   requires_fastcount,
//   # Time series collections do not support indexing array values in measurement fields.
//   exclude_from_timeseries_crud_passthrough,
// ]

const t = db[jsTestName()];
t.drop();

let N = 1000;

function pop() {
    t.drop();
    let arr = [];
    for (let i = 0; i < N; i++) {
        arr.push({x: 1, tags: ["a", "b", "c"]});
    }
    assert.commandWorked(t.insert(arr));
    assert.eq(t.count(), N);
}

function del() {
    return t.remove({tags: {$in: ["a", "c"]}});
}

function test(n, idx) {
    pop();
    assert.eq(N, t.count(), n + " A " + idx);
    if (idx) {
        assert.commandWorked(t.createIndex(idx));
    }
    let res = del();
    assert(!res.hasWriteError(), "error deleting: " + res.toString());
    assert.eq(0, t.count(), n + " B " + idx);
}

test("a");
test("b", {x: 1});
test("c", {tags: 1});

N = 5000;

test("a2");
test("b2", {x: 1});
test("c2", {tags: 1});
