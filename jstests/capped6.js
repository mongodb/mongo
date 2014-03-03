// Test NamespaceDetails::cappedTruncateAfter via 'captrunc' command

Random.setRandomSeed();

db.capped6.drop();
db._dbCommand( { create: "capped6", capped: true, size: 1000, $nExtents: 11, autoIndexId: false } );
tzz = db.capped6;

function debug( x ) {
//    print( x );
}

/**
 * Check that documents in the collection are in order according to the value
 * of a, which corresponds to the insert order.  This is a check that the oldest
 * document(s) is/are deleted when space is needed for the newest document.  The
 * check is performed in both forward and reverse directions.
 */
function checkOrder( i ) {
    res = tzz.find().sort( { $natural: -1 } );
    assert( res.hasNext(), "A" );
    var j = i;
    while( res.hasNext() ) {
        try {
            assert.eq( val[ j-- ].a, res.next().a, "B" );
        } catch( e ) {
            debug( "capped6 err " + j );
            throw e;
        }
    }
    res = tzz.find().sort( { $natural: 1 } );
    assert( res.hasNext(), "C" );
    while( res.hasNext() )
        assert.eq( val[ ++j ].a, res.next().a, "D" );
    assert.eq( j, i, "E" );
}

var val = new Array( 500 );
var c = "";
for( i = 0; i < 500; ++i, c += "-" ) {
    // The a values are strings of increasing length.
    val[ i ] = { a: c };
}

var oldMax = Random.randInt( 500 );
var max = 0;

/**
 * Insert new documents until there are 'oldMax' documents in the collection,
 * then remove a random number of documents (often all but one) via one or more
 * 'captrunc' requests.
 */
function doTest() {
    for( var i = max; i < oldMax; ++i ) {
        tzz.insert( val[ i ] );
    }
    max = oldMax;
    count = tzz.count();

    var min = 1;
    if ( Random.rand() > 0.3 ) {
        min = Random.randInt( count ) + 1;
    }

    // Iteratively remove a random number of documents until we have no more
    // than 'min' documents.
    while( count > min ) {
        // 'n' is the number of documents to remove - we must account for the
        // possibility that 'inc' will be true, and avoid removing all documents
        // from the collection in that case, as removing all documents is not
        // allowed by 'captrunc'
        var n = Random.randInt( count - min - 1 ); // 0 <= x <= count - min - 1
        var inc = Random.rand() > 0.5;
        debug( count + " " + n + " " + inc );
        assert.commandWorked( db.runCommand( { captrunc:"capped6", n:n, inc:inc } ) );
        if ( inc ) {
            n += 1;
        }
        count -= n;
        max -= n;
        // Validate the remaining documents.
        checkOrder( max - 1 );
    }
}

// Repeatedly add up to 'oldMax' documents and then truncate the newest
// documents.  Newer documents take up more space than older documents.
for( var i = 0; i < 10; ++i ) {
    doTest();
}

// reverse order of values
var val = new Array( 500 );

var c = "";
for( i = 499; i >= 0; --i, c += "-" ) {
    val[ i ] = { a: c };
}
db.capped6.drop();
db._dbCommand( { create: "capped6", capped: true, size: 1000, $nExtents: 11, autoIndexId: false } );
tzz = db.capped6;

// Same test as above, but now the newer documents take less space than the
// older documents instead of more.
for( var i = 0; i < 10; ++i ) {
    doTest();
}

tzz.drop();
