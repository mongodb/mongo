
t = db.mod1;
t.drop();

t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 11 } );
t.save( { a : 20 } );
t.save( { a : "asd" } );
t.save( { a : "adasdas" } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "A1" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "A2" );
assert.eq( 6 , t.find( { a : { $mod : [ 10 , 1 ] } } ).explain().nscanned , "A3" );

t.ensureIndex( { a : 1 } );

assert.eq( 2 , t.find( "this.a % 10 == 1" ).itcount() , "B1" );
assert.eq( 2 , t.find( { a : { $mod : [ 10 , 1 ] } } ).itcount() , "B2" );

assert.eq( 1 , t.find( "this.a % 10 == 0" ).itcount() , "B3" );
assert.eq( 1 , t.find( { a : { $mod : [ 10 , 0 ] } } ).itcount() , "B4" );
assert.eq( 4 , t.find( { a : { $mod : [ 10 , 1 ] } } ).explain().nscanned , "B5" );

assert.eq( 1, t.find( { a: { $gt: 5, $mod : [ 10, 1 ] } } ).itcount() );