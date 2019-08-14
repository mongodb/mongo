// Tests that timestamps are inserted in increasing order. This test assumes that timestamps
// inserted within the same second will have increasing increment values, which may not be the case
// if the inserts are into a sharded collection.
// @tags: [assumes_unsharded_collection]
(function() {
"use strict";
const t = db.ts1;
t.drop();

const N = 20;

for (let i = 0; i < N; i++) {
    assert.commandWorked(t.insert({_id: i, x: new Timestamp()}));
    sleep(100);
}

function get(i) {
    return t.findOne({_id: i}).x;
}

function cmp(a, b) {
    if (a.t < b.t)
        return -1;
    if (a.t > b.t)
        return 1;

    return a.i - b.i;
}

for (let i = 0; i < N - 1; i++) {
    const a = get(i);
    const b = get(i + 1);
    assert.gt(
        0, cmp(a, b), `Expected ${tojson(a)} to be smaller than ${tojson(b)} (at iteration ${i})`);
}

assert.eq(N, t.find({x: {$type: 17}}).itcount());
assert.eq(0, t.find({x: {$type: 3}}).itcount());

assert.commandWorked(t.insert({_id: 100, x: new Timestamp(123456, 50)}));
const x = t.findOne({_id: 100}).x;
assert.eq(123456, x.t);
assert.eq(50, x.i);
}());
