// @tags: [
//   requires_non_retryable_writes,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

// Yield and delete test case for query optimizer cursor.  SERVER-4401

let t = db.jstests_distinct3;
t.drop();

t.createIndex({a: 1});
t.createIndex({b: 1});

let bulk = t.initializeUnorderedBulkOp();
for (let i = 0; i < 50; ++i) {
    for (let j = 0; j < 2; ++j) {
        bulk.insert({a: i, c: i, d: j});
    }
}
for (let i = 0; i < 100; ++i) {
    bulk.insert({b: i, c: i + 50});
}
assert.commandWorked(bulk.execute());

// Attempt to remove the last match for the {a:1} index scan while distinct is yielding.
let p = startParallelShell(function () {
    for (let i = 0; i < 100; ++i) {
        let bulk = db.jstests_distinct3.initializeUnorderedBulkOp();
        bulk.find({a: 49}).remove();
        for (let j = 0; j < 20; ++j) {
            bulk.insert({a: 49, c: 49, d: j});
        }
        assert.commandWorked(bulk.execute());
    }
});

for (let i = 0; i < 100; ++i) {
    let count = t.distinct("c", {$or: [{a: {$gte: 0}, d: 0}, {b: {$gte: 0}}]}).length;
    assert.gt(count, 100);
}

p();
