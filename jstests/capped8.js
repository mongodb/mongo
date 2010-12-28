// Test NamespaceDetails::cappedTruncateAfter with empty extents

Random.setRandomSeed();

t = db.jstests_capped8;

function debug( x ) {
//    printjson( x );
}        

/** Generate an object with a string field of specified length */
function obj( size ) {
    return {a:new Array( size + 1 ).toString()};;
}

function withinOne( a, b ) {
    assert( Math.abs( a - b ) <= 1, "not within one: " + a + ", " + b )
}

/**
 * Insert enough documents of the given size spec that the collection will
 * contain only documents having this size spec.
 */
function insertMany( size ) {
    // Add some variability, as the precise number can trigger different cases.
    n = 250 + Random.randInt( 10 );
    for( i = 0; i < n; ++i ) {
        t.save( obj( size ) );
        debug( t.count() );
    }
}

/**
 * Insert some documents in such a way that there may be an empty extent, then
 * truncate the capped collection.
 */
function insertAndTruncate( first ) {
    myInitialCount = t.count();
    // Insert enough documents to make the capped allocation loop over.
    insertMany( 50 );
    myFiftyCount = t.count();
    // Insert documents that are too big to fit in the smaller extents.
    insertMany( 2000 );
    myTwokCount = t.count();
    if ( first ) {
        initialCount = myInitialCount;
        fiftyCount = myFiftyCount;
        twokCount = myTwokCount;
        // Sanity checks for collection count
        assert( fiftyCount > initialCount );
        assert( fiftyCount > twokCount );
    } else {
        // Check that we are able to insert roughly the same number of documents
        // after truncating.  The exact values are slightly variable as a result
        // of the capped allocation algorithm.
        withinOne( initialCount, myInitialCount );
        withinOne( fiftyCount, myFiftyCount );
        withinOne( twokCount, myTwokCount );
    }
    count = t.count();
    // Check that we can truncate the collection successfully.
    assert.commandWorked( db.runCommand( { captrunc:"jstests_capped8", n:count - 1, inc:false } ) );
}

/** Test truncating and subsequent inserts */
function testTruncate() {
    insertAndTruncate( true );
    insertAndTruncate( false );
    insertAndTruncate( false );
}

t.drop();
db._dbCommand( { create:"jstests_capped8", capped: true, $nExtents: [ 10000, 10000, 1000 ] } );
testTruncate();

t.drop();
db._dbCommand( { create:"jstests_capped8", capped: true, $nExtents: [ 10000, 1000, 1000 ] } );
testTruncate();

t.drop();
db._dbCommand( { create:"jstests_capped8", capped: true, $nExtents: [ 10000, 1000 ] } );
testTruncate();

t.drop();
db._dbCommand( { create:"jstests_capped8", capped: true, $nExtents: [ 10000 ] } );
testTruncate();
