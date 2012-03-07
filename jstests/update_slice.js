
t = db.update_slice
t.drop();

o = { _id : 1 , a : [ 2 , 1 ] }
t.insert( o );

assert.eq( o , t.findOne() , "A1" );

// No slice, and slice on a non-existing value is fine.
t.update( {} , { $push : { a : 3 } } );
o.a.push( 3 );
assert.eq( o , t.findOne() , "B1" );

t.update( {}, { $push: { a: 4 }, $slice : { b: 4 } } );
o.a.push( 4 );
assert.eq( o , t.findOne() , "B2" );

// Keep the last three
t.update( {} , { $push : { a : 5 }, $slice : { a: -3 } } );
o.a = [ 3, 4, 5 ]
assert.eq( o , t.findOne() , "C1" );

// Keep the first three (discards the new push)
t.update( {} , { $push : { a : 6 }, $slice : { a: 3 } } );
assert.eq( o , t.findOne() , "D1" );

// Slice it down to less than it was from the beginning
t.update( {} , { $push : { a : 6 }, $slice : { a: 2 } } );
o.a = [ 3, 4 ]
assert.eq( o , t.findOne() , "E1" );

t.update( {} , { $push : { a : 6 }, $slice : { a: -1 } } );
o.a = [ 6 ]
assert.eq( o , t.findOne() , "E2" );

// Empty it
t.update( {} , { $push : { a : 7 }, $slice : { a: 0 } } );
o.a = [ ]
assert.eq( o , t.findOne() , "F1" );

// Have a higher limit that we will not reach
t.update( {} , { $push : { a : 1 }, $slice : { a: 5 } } );
o.a.push( 1 )
assert.eq( o , t.findOne() , "G1" );
t.update( {} , { $push : { a : 2 }, $slice : { a: -5 } } );
o.a.push( 2 )
assert.eq( o , t.findOne() , "G2" );
t.update( {} , { $push : { a : 3 }, $slice : { a: 5 } } );
o.a.push( 3 )
assert.eq( o , t.findOne() , "G3" );
t.update( {} , { $push : { a : 4 }, $slice : { a: -5 } } );
o.a.push( 4 )
assert.eq( o , t.findOne() , "G4" );

// Edge cases
t.update( {} , { $push : { a : 5 }, $slice : { a: 5 } } );
o.a.push( 5 )
assert.eq( o , t.findOne() , "H1" );

t.update( {} , { $push : { a : 6 }, $slice : { a: -6 } } );
o.a.push( 6 )
assert.eq( o , t.findOne() , "H2" );

// Slice when pushing multiple values
t.update( {}, { $pushAll: { a: [ 7, 8, 9, 10 ] }, $slice : { a: 8 } } );
assert.eq( [ 1, 2, 3, 4, 5, 6, 7, 8 ], t.findOne().a, "I1" );

t.update( {}, { $pushAll: { a: [ 9, 10, 11, 12 ] }, $slice : { a: -8 } } );
assert.eq( [ 5, 6, 7, 8, 9, 10, 11, 12 ], t.findOne().a, "I2" );

// Slice by itself doesn't do anything (yet). It needs another modifier.
t.update( {}, { $slice : { a: 4 } } );
assert.eq( [ 5, 6, 7, 8, 9, 10, 11, 12 ], t.findOne().a, "J1" );

t.update( {}, { $slice : { a: -4 } } );
assert.eq( [ 5, 6, 7, 8, 9, 10, 11, 12 ], t.findOne().a, "J2" );

// However, you can get the same effect by pushing nothing (and sliceing)
t.update( {}, { $pushAll : { a : [ ] }, $slice : { a: -4 } } );
assert.eq( [ 9, 10, 11, 12 ], t.findOne().a, "K1" );

// Also works with addToSet
t.update( {}, { $addToSet: { a: 12 }, $slice : { a: -6 } } );
assert.eq( [ 9, 10, 11, 12 ], t.findOne().a, "L1" );

t.update( {}, { $addToSet: { a: 13 }, $slice : { a: -6 } } );
assert.eq( [ 9, 10, 11, 12, 13 ], t.findOne().a, "L2" );

t.update( {}, { $addToSet: { a: 13 }, $slice : { a: -6 } } );
assert.eq( [ 9, 10, 11, 12, 13 ], t.findOne().a, "L3" );

t.update( {}, { $addToSet: { a: 14 }, $slice : { a: -6 } } );
assert.eq( [ 9, 10, 11, 12, 13, 14 ], t.findOne().a, "L4" );

t.update( {}, { $addToSet: { a: 14 }, $slice : { a: -6 } } );
assert.eq( [ 9, 10, 11, 12, 13, 14 ], t.findOne().a, "L5" );

t.update( {}, { $addToSet: { a: 15 }, $slice : { a: -6 } } );
assert.eq( [ 10, 11, 12, 13, 14, 15 ], t.findOne().a, "L6" );

t.update( {}, { $addToSet: { a: 16 }, $slice : { a: 3 } } );
assert.eq( [ 10, 11, 12 ], t.findOne().a, "L7" );

// Can slice an array that is newly created
t.update( {}, { $pushAll: { b : [1, 2, 3, 4, 5] },
                $slice : { b : -3 } } );
assert.eq( [ 3, 4, 5 ], t.findOne().b, "M1" );

// It can limit several arrays
t.update( {}, { $addToSet: { a : 13 }, $pushAll: { b : [ 6, 7, 8] },
                $slice : { a : -3, b : 4 } } );
assert.eq( [ 11, 12, 13 ], t.findOne().a, "N1" );
assert.eq( [ 3, 4, 5, 6 ], t.findOne().b, "N1" );

// The argument must be a number.
t.update( {}, { $push: { a: 1 }, $slice : { a: { b: 2 } } } );
assert.eq( db.getLastErrorObj().code, 16073 , "O1" )

