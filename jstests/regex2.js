
t = db.regex2;
t.drop();

t.save( { a : "test" } );
t.save( { a : "Test" } );

assert.eq( 2 , t.find().count() , "A" );
assert.eq( 1 , t.find( { a : "Test" } ).count() , "B" );
assert.eq( 1 , t.find( { a : "test" } ).count() , "C" );
assert.eq( 1 , t.find( { a : /Test/ } ).count() , "D" );
assert.eq( 1 , t.find( { a : /test/ } ).count() , "E" );
assert.eq( 2 , t.find( { a : /test/i } ).count() , "F" );


t.drop();

a = "\u0442\u0435\u0441\u0442";
b = "\u0422\u0435\u0441\u0442";

assert( ( new RegExp( a ) ).test( a ) , "B 1" );
assert( ! ( new RegExp( a ) ).test( b ) , "B 2" );
assert( ( new RegExp( a , "i" ) ).test( b ) , "B 3 " );

t.save( { a : a } );
t.save( { a : b } );


assert.eq( 2 , t.find().count() , "C A" );
assert.eq( 1 , t.find( { a : a } ).count() , "C B" );
assert.eq( 1 , t.find( { a : b } ).count() , "C C" );
assert.eq( 1 , t.find( { a : new RegExp( a ) } ).count() , "C D" );
assert.eq( 1 , t.find( { a : new RegExp( b ) } ).count() , "C E" );
assert.eq( 2 , t.find( { a : new RegExp( a , "i" ) } ).count() , "C F" );




