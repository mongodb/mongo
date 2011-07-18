// Test unique and dropDups index options.

t = db.jstests_unique2;

t.drop();

/* test for good behavior when indexing multikeys */

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});

t.ensureIndex({k:1}, {unique:true, dropDups:true});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.drop();

t.ensureIndex({k:1}, {unique:true});

t.insert({k:3});
t.insert({k:[2,3]});
assert( db.getLastError() );
t.insert({k:[4,3]});
assert( db.getLastError() );

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.dropIndexes();

t.insert({k:[2,3]});
t.insert({k:[4,3]});
assert( t.count() == 3 ) ;

// Trigger an error, so we can test n of getPrevError() later.
assert.throws( function() { t.find( {$where:'aaa'} ).itcount(); } );
assert( db.getLastError() );
assert.eq( 1, db.getPrevError().nPrev );

t.ensureIndex({k:1}, {unique:true, dropDups:true});
// Check error flag was not set SERVER-2054.
assert( !db.getLastError() );
// Check that offset of pervious error is correct.
assert.eq( 2, db.getPrevError().nPrev );

// Check the dups were dropped.
assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

// Check that a new conflicting insert will cause an error.
t.insert({k:[2,3]});
assert( db.getLastError() );

t.drop();

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});
assert( t.count() == 3 ) ;


// Now try with a background index op.

// Trigger an error, so we can test n of getPrevError() later.
assert.throws( function() { t.find( {$where:'aaa'} ).itcount(); } );
assert( db.getLastError() );
assert.eq( 1, db.getPrevError().nPrev );

t.ensureIndex({k:1}, {background:true, unique:true, dropDups:true});
// Check error flag was not set SERVER-2054.
assert( !db.getLastError() );
// Check that offset of pervious error is correct.
assert.eq( 2, db.getPrevError().nPrev );

// Check the dups were dropped.
assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

// Check that a new conflicting insert will cause an error.
t.insert({k:[2,3]});
assert( db.getLastError() );
