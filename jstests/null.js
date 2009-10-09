
t = db.null1;
t.drop();

t.save( { x : 1 } );
t.save( { x : null } );

assert.eq( 1 , t.find( { x : null } ).count() , "A" );
assert.eq( 1 , t.find( { x : { $ne : null } } ).count() , "B" );

t.ensureIndex( { x : 1 } );

assert.eq( 1 , t.find( { x : null } ).count() , "C" );
assert.eq( 1 , t.find( { x : { $ne : null } } ).count() , "D" );
