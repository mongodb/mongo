
t = db.count3;

t.drop();

t.save( { a : 1 } );
t.save( { a : 1 , b : 2 } );

assert.eq( 2 , t.find( { a : 1 } ).itcount() , "A" );
assert.eq( 2 , t.find( { a : 1 } ).count() , "B" );

assert.eq( 2 , t.find( { a : 1 } , { b : 1 } ).itcount() , "C" );
assert.eq( 2 , t.find( { a : 1 } , { b : 1 } ).count() , "D" );

t.drop();

t.save( { a : 1 } );

assert.eq( 1 , t.find( { a : 1 } ).itcount() , "E" );
assert.eq( 1 , t.find( { a : 1 } ).count() , "F" );

assert.eq( 1 , t.find( { a : 1 } , { b : 1 } ).itcount() , "G" );
assert.eq( 1 , t.find( { a : 1 } , { b : 1 } ).count() , "H" );



