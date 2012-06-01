
t = db.bitwise1;
t.drop();

FLAG_A = 0x1;
FLAG_B = 0x2;
FLAG_C = 0x4;
FLAG_D = 0x8;
FLAG_F = 0xfffffff8; // 29 high bits set

t.save( { a : 0 } );
t.save( { a : FLAG_A } );
t.save( { a : FLAG_B, name: "upd" } );
t.save( { a : FLAG_C } );
t.save( { a : FLAG_D } );
t.save( { a : FLAG_C|FLAG_A } );
t.save( { a : FLAG_F } );
t.save( { a : "foobar" } );

// bitAnd
assert.eq( 2 , t.find( { a: { $bitAnd: FLAG_C } } ).itcount() , "A1" );
assert.eq( 3 , t.find( { a: { $bitAnd: FLAG_A|FLAG_C } } ).itcount() , "A2" ); // A,C,A|C
assert.eq( 1 , t.find( { a: { $bitAnd: FLAG_B } } ).itcount() , "A3" );
assert.eq( 4 , t.find( { a: { $bitAnd: FLAG_A|FLAG_B|FLAG_C } } ).itcount() , "A4" );
assert.eq( 0 , t.find( { a: { $bitAnd: 0 } } ).itcount() , "A5" );
assert.eq( 2 , t.find( { a: { $bitAnd: FLAG_D } } ).itcount() , "A6" ); // D and F
assert.eq( 1 , t.find( { a: { $bitAnd: 16 } } ).itcount() , "A7" ); // F
assert.eq( 1 , t.find( { a: { $bitAnd: 2147483648 } } ).itcount() , "A8" ); // F

// combined
assert.eq( 2, t.find( { a: { $gt: 1, $bitAnd: FLAG_C } } ).itcount(), "C1" ); // C, A|C
