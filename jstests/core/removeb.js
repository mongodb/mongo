// @tags: [
//   requires_fastcount,
//   requires_non_retryable_writes,
//   uses_multiple_connections,
// ]

// Test removal of Records that have been reused since the remove operation began.  SERVER-5198

(function() {
"use strict";

const t = db.jstests_removeb;
t.drop();

t.ensureIndex({a: 1});

// Make the index multikey to trigger cursor dedup checking.
t.insert({a: [-1, -2]});
t.remove({});

const insertDocs = function(collection, nDocs) {
    print("Bulk inserting " + nDocs + " documents");

    const bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < nDocs; ++i) {
        bulk.insert({a: i});
    }

    assert.commandWorked(bulk.execute());

    print("Bulk insert " + nDocs + " documents completed");
};

insertDocs(t, 20000);

const p = startParallelShell(function() {
    // Wait until the remove operation (below) begins running.
    while (db.jstests_removeb.count() === 20000) {
    }

    // Insert documents with increasing 'a' values.  These inserted documents may
    // reuse Records freed by the remove operation in progress and will be
    // visited by the remove operation if it has not completed.
    for (let i = 20000; i < 40000; i += 100) {
        const bulk = db.jstests_removeb.initializeUnorderedBulkOp();
        for (let j = 0; j < 100; ++j) {
            bulk.insert({a: i + j});
        }
        assert.commandWorked(bulk.execute());
        if (i % 1000 === 0) {
            print(i - 20000 + " of second set of 20000 documents inserted");
        }
    }
});

// Remove using the a:1 index in ascending direction.
var res = t.remove({a: {$gte: 0}});
assert(!res.hasWriteError(), 'The remove operation failed.');

p();

t.drop();
})();
