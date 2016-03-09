// Test that a memory exception is triggered for in memory sorts, but not for indexed sorts.

t = db.jstests_sortg;
t.drop();

big = new Array(1000000).toString();

for (i = 0; i < 100; ++i) {
    t.save({b: 0});
}

for (i = 0; i < 40; ++i) {
    t.save({a: 0, x: big});
}

function memoryException(sortSpec, querySpec) {
    querySpec = querySpec || {};
    var ex = assert.throws(function() {
        t.find(querySpec).sort(sortSpec).batchSize(1000).itcount();
    });
    assert(ex.toString().match(/Sort/));
}

function noMemoryException(sortSpec, querySpec) {
    querySpec = querySpec || {};
    t.find(querySpec).sort(sortSpec).batchSize(1000).itcount();
}

// Unindexed sorts.
memoryException({a: 1});
memoryException({b: 1});

// Indexed sorts.
noMemoryException({_id: 1});
noMemoryException({$natural: 1});

assert.eq(1, t.getIndexes().length);

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.ensureIndex({c: 1});

assert.eq(4, t.getIndexes().length);

// These sorts are now indexed.
noMemoryException({a: 1});
noMemoryException({b: 1});

// A memory exception is triggered for an unindexed sort involving multiple plans.
memoryException({d: 1}, {b: null, c: null});

// With an indexed plan on _id:1 and an unindexed plan on b:1, the indexed plan
// should succeed even if the unindexed one would exhaust its memory limit.
noMemoryException({_id: 1}, {b: null});

// With an unindexed plan on b:1 recorded for a query, the query should be
// retried when the unindexed plan exhausts its memory limit.
noMemoryException({_id: 1}, {b: null});
t.drop();
