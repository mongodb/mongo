
t = db.capped_empty;
t.drop();

db.createCollection( t.getName() , { capped : true , size : 100 } )

t.insert( { x : 1 } );
t.insert( { x : 2 } );
t.insert( { x : 3 } );
t.ensureIndex( { x : 1 } );

assert.eq( 3 , t.count() );
assert.eq( 1 , t.find( { x : 2 } ).explain().nscanned );

t.runCommand( "emptycapped" );

assert.eq( 0 , t.count() );

t.insert( { x : 1 } );
t.insert( { x : 2 } );
t.insert( { x : 3 } );

assert.eq( 3 , t.count() );
assert.eq( 1 , t.find( { x : 2 } ).explain().nscanned );
