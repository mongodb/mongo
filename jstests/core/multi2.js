
t = db.multi2;
t.drop();

t.save( { x : 1 , a : [ 1 ] } );
t.save( { x : 1 , a : [] } );
t.save( { x : 1 , a : null } );
t.save( {} );

assert.eq( 3 , t.find( { x : 1 } ).count() , "A" );

t.ensureIndex( { x : 1 } );
assert.eq( 3 , t.find( { x : 1 } ).count() , "B" );
assert.eq( 4 , t.find().sort( { x : 1 , a : 1 } ).count() , "s1" );
assert.eq( 1 , t.find( { x : 1 , a : null } ).count() , "B2" );

t.dropIndex( { x : 1 } );
t.ensureIndex( { x : 1 , a : 1 } );
assert.eq( 3 , t.find( { x : 1 } ).count() , "C" ); // SERVER-279
assert.eq( 4 , t.find().sort( { x : 1 , a : 1 } ).count() , "s2" );
assert.eq( 1 , t.find( { x : 1 , a : null } ).count() , "C2" );


