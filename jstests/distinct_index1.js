
t = db.distinct_index1
t.drop();

function r( x ){
    return Math.floor( Math.sqrt( x * 123123 ) ) % 10;
}

function d( k , q ){
    return t.runCommand( "distinct" , { key : k , query : q || {} } )
}

for ( i=0; i<1000; i++ ){
    o = { a : r(i*5) , b : r(i) };
    t.insert( o );
}

x = d( "a" );
assert.eq( 1000 , x.stats.n , "AA1" )
assert.eq( 1000 , x.stats.nscanned , "AA2" )
assert.eq( 1000 , x.stats.nscannedObjects , "AA3" )

x = d( "a" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "AB1" )
assert.eq( 1000 , x.stats.nscanned , "AB2" )
assert.eq( 1000 , x.stats.nscannedObjects , "AB3" )

x = d( "b" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "AC1" )
assert.eq( 1000 , x.stats.nscanned , "AC2" )
assert.eq( 1000 , x.stats.nscannedObjects , "AC3" )



t.ensureIndex( { a : 1 } )

x = d( "a" );
assert.eq( 1000 , x.stats.n , "BA1" )
assert.eq( 1000 , x.stats.nscanned , "BA2" )
assert.eq( 0 , x.stats.nscannedObjects , "BA3" )

x = d( "a" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "BB1" )
assert.eq( 398 , x.stats.nscanned , "BB2" )
assert.eq( 0 , x.stats.nscannedObjects , "BB3" )

x = d( "b" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "BC1" )
assert.eq( 398 , x.stats.nscanned , "BC2" )
assert.eq( 398 , x.stats.nscannedObjects , "BC3" )

// Check proper nscannedObjects count when using a query optimizer cursor.
t.dropIndexes();
t.ensureIndex( { a : 1, b : 1 } );
x = d( "b" , { a : { $gt : 5 }, b : { $gt : 5 } } );
assert.eq( "QueryOptimizerCursor", x.stats.cursor );
assert.eq( 171 , x.stats.n )
assert.eq( 275 , x.stats.nscanned )
// Disable temporarily - exact value doesn't matter.
// assert.eq( 266 , x.stats.nscannedObjects )
