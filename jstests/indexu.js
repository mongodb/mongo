// Test index key generation with duplicate values addressed by array index and
// object field.  SERVER-2902

t = db.jstests_indexu;
t.drop();

var dupDoc = {a:[{'0':1}]}; // There are two 'a.0' fields in this doc.
var dupDoc2 = {a:[{'1':1},'c']};
var noDupDoc = {a:[{'1':1}]};

// Test that we can't index dupDoc.
t.save( dupDoc );
assert( !db.getLastError() );
t.ensureIndex( {'a.0':1} );
assert( db.getLastError() );

t.remove();
t.ensureIndex( {'a.0':1} );
assert( !db.getLastError() );
t.save( dupDoc );
assert( db.getLastError() );

// Test that we can't index dupDoc2.
t.drop();
t.save( dupDoc2 );
assert( !db.getLastError() );
t.ensureIndex( {'a.1':1} );
assert( db.getLastError() );

t.remove();
t.ensureIndex( {'a.1':1} );
assert( !db.getLastError() );
t.save( dupDoc2 );
assert( db.getLastError() );

// Test that we can index dupDoc with a different index.
t.drop();
t.ensureIndex( {'a.b':1} );
t.save( dupDoc );
assert( !db.getLastError() );

// Test number field starting with hyphen.
t.drop();
t.ensureIndex( {'a.-1':1} );
t.save( {a:[{'-1':1}]} );
assert( !db.getLastError() );

// Test number field starting with zero.
t.drop();
t.ensureIndex( {'a.00':1} );
t.save( {a:[{'00':1}]} );
assert( !db.getLastError() );

// Test multiple array indexes
t.drop();
t.ensureIndex( {'a.0':1,'a.1':1} );
t.save( {a:[{'1':1}]} );
assert( !db.getLastError() );
t.save( {a:[{'1':1},4]} );
assert( db.getLastError() );

// Test that we can index noDupDoc.
t.drop();
t.save( noDupDoc );
t.ensureIndex( {'a.0':1} );
assert( !db.getLastError() );
t.ensureIndex( {'a.1':1} );
assert( !db.getLastError() );

t.drop();
t.ensureIndex( {'a.0':1} );
t.ensureIndex( {'a.1':1} );
t.save( noDupDoc );
assert( !db.getLastError() );

// Test that we can query noDupDoc.
assert.eq( 1, t.find( {'a.1':1} ).hint( {'a.1':1} ).itcount() );
assert.eq( 1, t.find( {'a.1':1} ).hint( {$natural:1} ).itcount() );
assert.eq( 1, t.find( {'a.0':{'1':1}} ).hint( {'a.0':1} ).itcount() );
assert.eq( 1, t.find( {'a.0':{'1':1}} ).hint( {$natural:1} ).itcount() );

// Check multiple nested array fields.
t.drop();
t.save( {a:[[1]]} );
t.ensureIndex( {'a.0.0':1} );
assert( !db.getLastError() );
assert.eq( 1, t.find( {'a.0.0':1} ).hint( {$natural:1} ).itcount() );
assert.eq( 1, t.find( {'a.0.0':1} ).hint( {'a.0.0':1} ).itcount() );

// Check where there is a duplicate for a partially addressed field but not for a fully addressed field.
t.drop();
t.save( {a:[[1],{'0':1}]} );
t.ensureIndex( {'a.0.0':1} );
assert( db.getLastError() );

// Check where there is a duplicate for a fully addressed field.
t.drop();
t.save( {a:[[1],{'0':[1]}]} );
assert( !db.getLastError() );
t.ensureIndex( {'a.0.0':1} );
assert( db.getLastError() );

// Two ways of addressing parse to an array.
t.drop();
t.save( {a:[{'0':1}]} );
t.ensureIndex( {'a.0.0':1} );
assert( db.getLastError() );

// Test several key depths - with same arrays being found.
t.drop();
t.save( {a:[{'0':[{'0':1}]}]} );
t.ensureIndex( {'a.0.0.0.0.0.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a.0.0.0.0.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a.0.0.0.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a.0.0.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a.0.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a.0':1} );
assert( db.getLastError() );
t.ensureIndex( {'a':1} );
assert( !db.getLastError() );

// Two prefixes extract docs, but one terminates extraction before array.
t.drop();
t.save( {a:[{'0':{'c':[]}}]} );
t.ensureIndex( {'a.0.c':1} );
assert( db.getLastError() );

t.drop();
t.save( {a:[[{'b':1}]]} );
assert.eq( 1, t.find( {'a.0.b':1} ).itcount() );
t.ensureIndex( {'a.0.b':1} );
assert.eq( 1, t.find( {'a.0.b':1} ).itcount() );
