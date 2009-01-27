// Test different directions for compound indexes

function eq( one, two ) {
    assert.eq( one.a, two.a );
    assert.eq( one.b, two.b );
}

function checkExplain( e, idx, reverse, nScanned ) {
    if ( !reverse ) {
	if ( idx ) {
	    assert.eq( "BtreeCursor a_1_b_-1", e.cursor );
	} else {
	    assert.eq( "BasicCursor", e.cursor );
	}
    } else {
	if ( idx ) {
	    assert.eq( "BtreeCursor a_1_b_-1 reverse", e.cursor );
	} else {
	    assert( false );
	}
    }
    assert.eq( nScanned, e.nscanned );
}

function check( indexed ) {
    e = r.find().sort( { a: 1, b: 1 } ).explain();
    checkExplain( e, false, false, 4 );
    f = r.find().sort( { a: 1, b: 1 } );
    eq( z[ 0 ], f[ 0 ] );
    eq( z[ 1 ], f[ 1 ] );
    eq( z[ 2 ], f[ 2 ] );
    eq( z[ 3 ], f[ 3 ] );

    e = r.find().sort( { a: 1, b: -1 } ).explain();
    checkExplain( e, true && indexed, false, 4 );
    f = r.find().sort( { a: 1, b: -1 } );
    eq( z[ 1 ], f[ 0 ] );
    eq( z[ 0 ], f[ 1 ] );
    eq( z[ 3 ], f[ 2 ] );
    eq( z[ 2 ], f[ 3 ] );

    e = r.find().sort( { a: -1, b: 1 } ).explain();
    checkExplain( e, true && indexed, true && indexed, 4 );
    f = r.find().sort( { a: -1, b: 1 } );
    eq( z[ 2 ], f[ 0 ] );
    eq( z[ 3 ], f[ 1 ] );
    eq( z[ 0 ], f[ 2 ] );
    eq( z[ 1 ], f[ 3 ] );

    e = r.find( { a: { $gte: 2 } } ).sort( { a: 1, b: -1 } ).explain();
    checkExplain( e, true && indexed, false, indexed ? 2 : 4 );
    f = r.find( { a: { $gte: 2 } } ).sort( { a: 1, b: -1 } );
    eq( z[ 3 ], f[ 0 ] );
    eq( z[ 2 ], f[ 1 ] );

    e = r.find( { a : { $gte: 2 } } ).sort( { a: -1, b: 1 } ).explain();
    checkExplain( e, true && indexed, true && indexed, indexed ? 2 : 4 );
    f = r.find( { a: { $gte: 2 } } ).sort( { a: -1, b: 1 } );
    eq( z[ 2 ], f[ 0 ] );
    eq( z[ 3 ], f[ 1 ] );

    e = r.find().sort( { a: -1, b: -1 } ).explain();
    checkExplain( e, false, false, 4 );
    f = r.find().sort( { a: -1, b: -1 } );
    eq( z[ 3 ], f[ 0 ] );
    eq( z[ 2 ], f[ 1 ] );
    eq( z[ 1 ], f[ 2 ] );
    eq( z[ 0 ], f[ 3 ] );
}

db = connect( "test" );
db.setProfilingLevel( 1 );
r = db.ed_db_cursor6;
r.drop();

z = [ { a: 1, b: 1 },
      { a: 1, b: 2 },
      { a: 2, b: 1 },
      { a: 2, b: 2 } ];
for( i = 0; i < z.length; ++i )
    r.save( z[ i ] );

check( false );

r.ensureIndex( { a: 1, b: -1 } );

check( true );

assert.eq( "BasicCursor", r.find().sort( { a: 1, b: -1, z: 1 } ).explain().cursor );
