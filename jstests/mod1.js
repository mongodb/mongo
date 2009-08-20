
t = db.mod1;
t.drop();

t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 11 } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "A" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "B" );

t.ensureIndex( { a : 1 } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "C" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "D" );

