db.capped6.drop();
db._dbCommand( { create: "capped6", capped: true, size: 1000, $nExtents: 11, autoIndexId: false } );
tzz = db.capped6;
tzz.ensureIndex( {i:1}, {unique:true} );

function debug( x ) {
//    print( x );
}

var val = new Array( 2000 );
var c = "";
for( i = 0; i < 2000; ++i, c += "-" ) {
    val[ i ] = { a: c };
}

function checkIncreasing( i ) {
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

function checkDecreasing( i ) {
    res = tzz.find().sort( { $natural: -1 } );
    assert( res.hasNext(), "F" );
    var j = i;
    while( res.hasNext() ) {
      	assert.eq( val[ j++ ].a, res.next().a, "G" );
    }
    res = tzz.find().sort( { $natural: 1 } );
    assert( res.hasNext(), "H" );
    while( res.hasNext() )
        assert.eq( val[ --j ].a, res.next().a, "I" );
    assert.eq( j, i, "J" );
}

for( i = 0 ;; ++i ) {
    debug( "capped 6: " + i );
    tzz.save( {a:val[ i ].a, i:i } );
    if ( i % 5 == 2 ) {
        if( 1 == tzz.count( {i:i-2} ) ) {
            tzz.save( {a:val[ i ].a, i:i-2 } ); // this is a duplicate for i & will fail
            assert.eq( 1, tzz.count( {i:i-2} ) );
        }
    }
    if ( tzz.count() == 0 ) {
        assert( i > 100, "K" );
        break;
    }
    checkIncreasing( i );
}

for( i = 600 ; i >= 0 ; --i ) {
    debug( "capped 6: " + i );
    tzz.save( {a:val[ i ].a, i:i } );
    if ( i % 5 == 0 ) {
        tzz.save( {a:val[ i ].a, i:i } ); // this is a duplicate for i & will fail
    }
    checkDecreasing( i );
}
