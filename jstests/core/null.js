
t = db.null1;
t.drop();

t.save( { x : 1 } );
t.save( { x : null } );

assert.eq( 1 , t.find( { x : null } ).count() , "A" );
assert.eq( 1 , t.find( { x : { $ne : null } } ).count() , "B" );

t.ensureIndex( { x : 1 } );

assert.eq( 1 , t.find( { x : null } ).count() , "C" );
assert.eq( 1 , t.find( { x : { $ne : null } } ).count() , "D" );

// -----

assert.eq( 2, t.find( { y : null } ).count(), "E" );

t.ensureIndex( { y : 1 } );
assert.eq( 2, t.find( { y : null } ).count(), "E" );

t.dropIndex( { y : 1 } );

t.ensureIndex( { y : 1 }, { sparse : true } );
assert.eq( 2, t.find( { y : null } ).count(), "E" );
