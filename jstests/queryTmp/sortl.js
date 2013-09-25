// When one plan among candidate plans of mixed ordering types is cached, and then replayed, and the
// remaining plans are then attempted, those remaining plans are iterated properly for their
// ordering type.  SERVER-5301

t = db.jstests_sortl;
t.drop();

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

function recordIndex( index, query, sort ) {
    // Run a query that records the desired index.
    t.find( query ).sort( sort ).explain();
    // Check that the desired index is recorded.
    assert.eq( 'BtreeCursor ' + index,
              t.find( query ).sort( sort ).explain( true ).oldPlan.cursor );
}

function checkBOrdering( result ) {
    for( i = 1; i < result.length; ++i ) {
        assert.lt( result[ i - 1 ].b, result[ i ].b );
    }
}

// An out of order plan is recorded, then an in order plan takes over.
t.save( { a:1 } );
big = new Array( 1000000 ).toString();
for( i = 0; i < 40; ++i ) {
    t.save( { a:2, b:i, c:big } );
}

recordIndex( 'a_1', { a:1 }, { b:1 } );
result = t.find( { a:2 }, { a:1, b:1 } ).sort( { b:1 } ).toArray();
assert.eq( 40, result.length );
checkBOrdering( result );

// An optimal in order plan is recorded and reused.
recordIndex( 'b_1', { b:{ $gte:0 } }, { b:1 } );
result = t.find( { b:{ $gte:0 } }, { b:1 } ).sort( { b:1 } ).toArray();
assert.eq( 40, result.length );
checkBOrdering( result );

t.remove();

// An in order plan is recorded, then an out of order plan is added.
for( i = 0; i < 20; ++i ) {
    t.save( { a:1, b:19-i } );
}

recordIndex( 'b_1', { a:1, b:{ $gte:19 } }, { b:1 } );
result = t.find( { a:1, b:{ $gte:0 } } ).sort( { b:1 } ).toArray();
assert.eq( 20, result.length );
checkBOrdering( result );
