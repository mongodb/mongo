
t = db.getCollection( "where1" );
t.drop();

t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 3 } );

assert.eq( 1 , t.find( { $where : "this.a == 2" } ).toArray().length , "A" );
assert.eq( 1 , t.find( { $where : "\nthis.a == 2" } ).toArray().length , "B" );
