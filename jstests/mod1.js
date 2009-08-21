
t = db.mod1;
t.drop();

t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 11 } );
t.save( { a : 20 } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "A" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "B" );

t.ensureIndex( { a : 1 } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "C" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "D" );

assert.eq( 1 , t.find( "this.a % 10 == 0" ).itcount() , "E" );
assert.eq( 1 , t.find( { a : { $mod : [ 10 , 0 ] } } ).itcount() , "F" );


