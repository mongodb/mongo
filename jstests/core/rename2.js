

a = db.rename2a;
b = db.rename2b;

a.drop();
b.drop();

a.save( { x : 1 } )
a.save( { x : 2 } )
a.save( { x : 3 } )

assert.eq( 3 , a.count() , "A" )
assert.eq( 0 , b.count() , "B" )

assert( a.renameCollection( "rename2b" ) , "the command" );

assert.eq( 0 , a.count() , "C" )
assert.eq( 3 , b.count() , "D" )
