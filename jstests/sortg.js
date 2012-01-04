// Test that a memory exception is triggered for in memory sorts, but not for indexed sorts.

t = db.jstests_sortg;
t.drop();

big = new Array( 1000000 ).toString()

for( i = 0; i < 40; ++i ) {
    t.save({a:big});
}

function memoryException( sortSpec, querySpec ) {
    querySpec = querySpec || {};
    assert.throws( function() { t.find( querySpec ).sort( sortSpec ).itcount() } );
    assert( db.getLastError().match( /too much data for sort\(\) with no index/ ) );
}

function noMemoryException( sortSpec, querySpec ) {
    querySpec = querySpec || {};
    t.find( querySpec ).sort( sortSpec ).itcount();
    assert( !db.getLastError() );
}

// Unindexed sorts.
memoryException( {a:1} );
memoryException( {b:1} );

// Indexed sorts.
noMemoryException( {_id:1} );
noMemoryException( {$natural:1} );

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

// These sorts are now indexed.
noMemoryException( {a:1} );
noMemoryException( {b:1} );

// With an indexed plan on _id:1 and an unindexed plan on b:1, the indexed plan
// should succeed even if the unindexed one exhausts its memory limit.
noMemoryException( {_id:1}, {b:null} );

// With an unindexed plan on b:1 recorded for a query, the query should be
// retried when the unindexed plan exhausts its memory limit.
assert.eq( 'BtreeCursor b_1', t.find( {b:1} ).sort( {_id:1} ).explain().cursor );
noMemoryException( {_id:1}, {b:null} );
