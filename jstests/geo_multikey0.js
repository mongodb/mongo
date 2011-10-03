// Multikey geo values tests - SERVER-3793.

t = db.jstests_geo_multikey0;
t.drop();

// Check that conflicting constraints are satisfied by parallel array elements.
t.save( {loc:[{x:20,y:30},{x:30,y:40}]} );
assert.eq( 1, t.count( {loc:{x:20,y:30},$and:[{loc:{$gt:{x:20,y:35},$lt:{x:20,y:34}}}]} ) );

// Check that conflicting constraints are satisfied by parallel array elements with a 2d index on loc.
if ( 0 ) { // SERVER-3793
t.ensureIndex( {loc:'2d'} );
assert.eq( 1, t.count( {loc:{x:20,y:30},$and:[{loc:{$gt:{x:20,y:35},$lt:{x:20,y:34}}}]} ) );
}

t.drop();

// Check that conflicting constraints are satisfied by parallel array elements of x.
t.save( {loc:[20,30],x:[1,2]} );
assert.eq( 1, t.count( {loc:[20,30],x:{$gt:1.7,$lt:1.2}} ) );

// Check that conflicting constraints are satisfied by parallel array elements of x with a 2d index on loc,x.
if ( 0 ) { // SERVER-3793
t.ensureIndex( {loc:'2d',x:1} );
assert.eq( 1, t.count( {loc:[20,30],x:{$gt:1.7,$lt:1.2}} ) );
}
