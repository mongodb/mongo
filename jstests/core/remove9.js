// @tags: [
//   requires_getmore,
//   requires_non_retryable_writes,
//   uses_multiple_connections,
// ]

// SERVER-2009 Count odd numbered entries while updating and deleting even numbered entries.

(function() {
"use strict";

const t = db.jstests_remove9;
t.drop();
t.ensureIndex({i: 1});

const bulk = t.initializeUnorderedBulkOp();
for (let i = 0; i < 1000; ++i) {
    bulk.insert({i: i});
}
assert.commandWorked(bulk.execute());

const s = startParallelShell(function() {
    const t = db.jstests_remove9;
    Random.setRandomSeed();
    for (let j = 0; j < 5000; ++j) {
        const i = Random.randInt(499) * 2;
        t.update({i: i}, {$set: {i: 2000}});
        t.remove({i: 2000});
        t.save({i: i});
    }
});

for (let i = 0; i < 1000; ++i) {
    assert.eq(500, t.find({i: {$gte: 0, $mod: [2, 1]}}).hint({i: 1}).itcount());
}

s();
})();
