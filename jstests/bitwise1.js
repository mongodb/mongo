
t = db.bitwise1;
t.drop();

t.save( { a : 0 } );
t.save( { a : 1 } );
t.save( { a : 2 } );
t.save( { a : 4 } );
t.save( { a : 5 } );
t.save( { a : 4294967288 } ); // 29 high bits set
t.save( { a : "foobar" } );

// bitand
assert.eq( 2 , t.find( { a: { $bitand: 4 } } ).itcount() , "A1" ); // 4,5
assert.eq( 3 , t.find( { a: { $bitand: 5 } } ).itcount() , "A2" ); // 1,4,5
assert.eq( 1 , t.find( { a: { $bitand: 2 } } ).itcount() , "A3" ); // 2
assert.eq( 4 , t.find( { a: { $bitand: 7 } } ).itcount() , "A4" ); // 1,2,4,5
assert.eq( 0 , t.find( { a: { $bitand: 0 } } ).itcount() , "A5" );
assert.eq( 1 , t.find( { a: { $bitand: 8 } } ).itcount() , "A6" ); // 429..
assert.eq( 1 , t.find( { a: { $bitand: 16 } } ).itcount() , "A7" ); // 429..
assert.eq( 1 , t.find( { a: { $bitand: 2147483648 } } ).itcount() , "A8" ); // 429..

// bitor
assert.eq( 6 , t.find( { a: { $bitor: 1 } } ).itcount() , "B1" );
assert.eq( 5 , t.find( { a: { $bitor: 0 } } ).itcount() , "B2" );

