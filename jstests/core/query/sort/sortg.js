// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
//   requires_getmore,
// ]

// Test that a memory exception is triggered for in memory sorts, but not for indexed sorts.

// TODO SERVER-92452: This test fails in burn-in with the 'inMemory' engine with the 'WT_CACHE_FULL'
// error. This is a known issue and can be ignored. Remove this comment once SERVER-92452 is fixed.

const t = db.jstests_sortg;
t.drop();

const big = new Array(1000000).toString();

let i;
for (i = 0; i < 100; ++i) {
    t.save({b: 0});
}

for (i = 0; i < 110; ++i) {
    t.save({a: 0, x: big});
}

function memoryException(sortSpec, querySpec) {
    querySpec = querySpec || {};
    let ex = assert.throwsWithCode(
        () => t.find(querySpec).sort(sortSpec).allowDiskUse(false).batchSize(1000).itcount(),
        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
    );
    assert(ex.toString().match(/Sort/));
}

function noMemoryException(sortSpec, querySpec) {
    querySpec = querySpec || {};
    t.find(querySpec).sort(sortSpec).allowDiskUse(false).batchSize(1000).itcount();
}

// Unindexed sorts.
memoryException({a: 1});
memoryException({b: 1});

// Indexed sorts.
noMemoryException({_id: 1});
noMemoryException({$natural: 1});

assert.eq(1, t.getIndexes().length);

t.createIndex({a: 1});
t.createIndex({b: 1});
t.createIndex({c: 1});

assert.eq(4, t.getIndexes().length);

// These sorts are now indexed.
noMemoryException({a: 1});
noMemoryException({b: 1});

// A memory exception is triggered for an unindexed sort involving multiple plans.
memoryException({d: 1}, {b: null, c: null});

// With an indexed plan on _id:1 and an unindexed plan on b:1, the indexed plan should succeed
// even if the unindexed one would exhaust its memory limit.
noMemoryException({_id: 1}, {b: null});

// With an unindexed plan on b:1 recorded for a query, the query should be retried when the
// unindexed plan exhausts its memory limit.
noMemoryException({_id: 1}, {b: null});
t.drop();
