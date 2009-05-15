
t = db.getCollection( "where1" );
t.drop();

t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 3 } );

assert.eq( 1 , t.find( function(){ return this.a == 2; } ).length() , "A" );

assert.eq( 1 , t.find( { $where : "return this.a == 2" } ).toArray().length , "B" );
assert.eq( 1 , t.find( { $where : "this.a == 2" } ).toArray().length , "C" );

assert.eq( 1 , t.find( "this.a == 2" ).toArray().length , "D" );
