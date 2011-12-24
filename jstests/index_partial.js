// index_filter.js Test partial indexes
function index( q ) {
    assert( q.explain().cursor.match( /^BtreeCursor/ ) , "index assert" );
}

function noIndex( q ) {
    assert( q.explain().cursor.match( /^BasicCursor/ ) , "noIndex assert" );
}

f = db.db_index_filter;
f.drop();

f.save( { a : 5, x: 3 } )
f.save( { a : 2, x: 6 } )
f.save( { a : 4, x: 6 } )
f.save( { a : 10 } )
f.save( { a : 10, x: 10 } )
f.save( { x: 6 } )
f.ensureIndex( { a: 1 }, { filter : {x:6}} );

// It shouldn't use the partial index because filter criteria is not used in the query
noIndex( f.find( { a: 5 } ) );
noIndex( f.find( { a: 5, x: 3 } ) );

// It shouldn't use the partial index because indexed field is not present
noIndex( f.find( { x: 6 } ) );

// It should use the partial index
index(f.find( { a: 2, x: 6 } ) );
assert.eq(f.find( { a: 2, x: 6 } ).itcount(), 1)

index(f.find( { a: 3, x: 6 } ) );
assert.eq(f.find( { a: 3, x: 6 } ).itcount(), 0)

index(f.find( { a: {$gt:0}, x: 6 } ) );
assert.eq(f.find( { a: {$gt:0}, x: 6 } ).itcount(), 2)

// It should use the partial index (inequality implications in queries)
index(f.find( { a: 2, x: {$gt:0} } ) );
assert.eq(f.find( { a: 2, x: {$gt:0} } ).itcount(), 1)

noIndex(f.find( { a: 2, x: {$gt:10} } ) );
assert.eq(f.find( { a: 2, x: {$gt:10} } ).itcount(), 0)

index(f.find( { a: 2, x: {$lt:10} } ) );
assert.eq(f.find( { a: 2, x: {$lt:10} } ).itcount(), 1)

noIndex(f.find( { a: 2, x: {$lt:2} } ) );
assert.eq(f.find( { a: 2, x: {$lt:2} } ).itcount(), 0)

// It should use the partial index (inequality implications in index filter)

f.dropIndex( { a: 1 } );
f.ensureIndex( { a: 1 }, { filter : {x:{$gt:4}}} );

index(f.find( { a: 10, x: 10 } ) );
assert.eq(f.find( { a: 10, x: 10 } ).itcount(), 1)

index(f.find( { a: 10, x: {$gt:7} } ) );
assert.eq(f.find( { a: 10, x: {$gt:7} } ).itcount(), 1)

// It shouldn't use the partial index when sorting
noIndex(f.find().sort( { a: 1 } ) );
noIndex(f.find().sort( { x: 1 } ) );
noIndex(f.find().sort( { a: 1, x: 1 } ) );

// It should use the partial index in case of several fields in the condition

f.drop();

f.save( { a : 5, x: 3, y: 1 } )
f.save( { a : 2, x: 6, y: 0 } )
f.save( { a : 4, x: 6, y: 1 } )
f.save( { a : 3, x: 6 } )

f.dropIndex( { a: 1 } ); // todo, useless
f.ensureIndex( { a: 1 }, { filter : {x: 6, y: 1}} );

// Search including the indexed field (a), and the same condition query than the index condition
index(f.find( { a: 4, x: 6, y: 1 } ) );
assert.eq(f.find( {  a: 4, x: 6, y: 1 } ).itcount(), 1)

// One of the fields in the condition is not the same than filter in the index
noIndex(f.find( { a: 2, x: 6, y: 0 } ) );
assert.eq(f.find( {  a: 4, x: 6, y: 0 } ).itcount(), 0)

// Field indexed not included in the query
noIndex(f.find( { x: 6, y: 1 } ) );
assert.eq(f.find( { x: 6, y: 1 } ).itcount(), 1)

// All fields from index condition are not present in the find query
noIndex(f.find( {a: 4, x: 6 } ) );
assert.eq(f.find( { a: 4, x: 6} ).itcount(), 1)

// Search including the indexed field, and with a query included in the index condition  (inequality implications
f.dropIndex( { a: 1 } );
f.ensureIndex( { a: 1 }, { filter : {x: {$gt:0}, y: 1}} );

index(f.find( { a: 4, x: 6, y: 1 } ) );
assert.eq(f.find( {  a: 4, x: 6, y: 1 } ).itcount(), 1)

index(f.find( { a: {$gt:2}, x: {$gt:2}, y: 1 } ) );
assert.eq(f.find( {  a: {$gt:2}, x: {$gt:2}, y: 1 } ).itcount(), 2)

index(f.find( { a: {$gt:2}, x: {$gt:2}, y: 1, z: 0 } ) );
assert.eq(f.find( {  a: {$gt:2}, x: {$gt:2}, y: 1, z: 0 } ).itcount(), 0)
