// Test unique and dropDups index options.

function checkNprev( np ) {
    // getPrevError() is not available sharded.
    if ( typeof( myShardingTest ) == 'undefined' ) {
        assert.eq( np, db.getPrevError().nPrev );
    }
}

t = db.jstests_unique2;

t.drop();

/* test for good behavior when indexing multikeys */

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});
t.insert({k:[4,3]}); // tests SERVER-4770

t.ensureIndex({k:1}, {unique:true, dropDups:true});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.drop();

/* same test wtih background:true*/

t.insert({k:3});
t.insert({k:[2,3]});
t.insert({k:[4,3]});
t.insert({k:[4,3]}); 

t.ensureIndex({k:1}, {unique:true, dropDups:true, background:true});

assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

t.drop();

/* */

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
checkNprev( 1 );

t.ensureIndex({k:1}, {unique:true, dropDups:true});
// Check error flag was not set SERVER-2054.
assert( !db.getLastError() );
// Check that offset of previous error is correct.
checkNprev( 2 );

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
checkNprev( 1 );

t.ensureIndex({k:1}, {background:true, unique:true, dropDups:true});
// Check error flag was not set SERVER-2054.
assert( !db.getLastError() );
// Check that offset of pervious error is correct.
checkNprev( 2 );

// Check the dups were dropped.
assert( t.count() == 1 ) ;
assert( t.find().sort({k:1}).toArray().length == 1 ) ;
assert( t.find().sort({k:1}).count() == 1 ) ;

// Check that a new conflicting insert will cause an error.
t.insert({k:[2,3]});
assert( db.getLastError() );
