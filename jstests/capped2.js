db.capped2.drop();
db._dbCommand( { create: "capped2", capped: true, size: 1000, $nExtents: 11, autoIndexId: false } );
t = db.capped2;

var val = new Array( 2000 );
var c = "";
for( i = 0; i < 2000; ++i, c += "-" ) {
    val[ i ] = { a: c };
}

function checkIncreasing( i ) {
    res = t.find().sort( { $natural: -1 } );
    assert( res.hasNext() );
    var j = i;
    while( res.hasNext() ) {
      	assert.eq( val[ j-- ].a, res.next().a );
    }
    res = t.find().sort( { $natural: 1 } );
    assert( res.hasNext() );
    while( res.hasNext() )
	assert.eq( val[ ++j ].a, res.next().a );
    assert.eq( j, i );
}

function checkDecreasing( i ) {
    res = t.find().sort( { $natural: -1 } );
    assert( res.hasNext() );
    var j = i;
    while( res.hasNext() ) {
      	assert.eq( val[ j++ ].a, res.next().a );
    }
    res = t.find().sort( { $natural: 1 } );
    assert( res.hasNext() );
    while( res.hasNext() )
	assert.eq( val[ --j ].a, res.next().a );
    assert.eq( j, i );
}

for( i = 0 ;; ++i ) {
    t.save( val[ i ] );
    if ( t.count() == 0 ) {
	assert( i > 100 );
	break;
    }
    checkIncreasing( i );
}

for( i = 600 ; i >= 0 ; --i ) {
    t.save( val[ i ] );
    checkDecreasing( i );
}
