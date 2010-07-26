Random.setRandomSeed();

db.capped6.drop();
db._dbCommand( { create: "capped6", capped: true, size: 1000, $nExtents: 11, autoIndexId: false } );
tzz = db.capped6;

function debug( x ) {
//    print( x );
}

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
    val[ i ] = { a: c };
}

var oldMax = Random.randInt( 500 );
var max = 0;

function doTest() {
    for( var i = max; i < oldMax; ++i ) {
        tzz.save( val[ i ] );
    }
    max = oldMax;
    count = tzz.count();

    var min = 1;
    if ( Random.rand() > 0.3 ) {
        min = Random.randInt( count ) + 1;
    }

    while( count > min ) {
        var n = Random.randInt( count - min - 1 ); // 0 <= x <= count - min - 1
        var inc = Random.rand() > 0.5;
        debug( count + " " + n + " " + inc );
        assert.commandWorked( db.runCommand( { captrunc:"capped6", n:n, inc:inc } ) );
        if ( inc ) {
            n += 1;
        }
        count -= n;
        max -= n;
        checkOrder( max - 1 );
    }
}

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

for( var i = 0; i < 10; ++i ) {
    doTest();
}
