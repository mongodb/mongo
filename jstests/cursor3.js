// Test inequality bounds combined with ordering for a single-field index.
// BUG 1079 (fixed)

function checkResults( expected, cursor ) {
    assert.eq( expected.length, cursor.count() );
    for( i = 0; i < expected.length; ++i ) {
	assert.eq( expected[ i ], cursor[ i ][ "a" ] );
    }
}

function testConstrainedFindWithOrdering( db ) {
    r = db.ed_db_cursor3_cfwo;
    r.drop()

    r.save( { a: 0 } );
    r.save( { a: 1 } );
    r.save( { a: 2 } );
    r.ensureIndex( { a: 1 } );

    checkResults( [ 1 ], r.find( { a: 1 } ).sort( { a: 1 } ).hint( { a: 1 } ) );
    checkResults( [ 1 ], r.find( { a: 1 } ).sort( { a: -1 } ).hint( { a: 1 } ) );

    checkResults( [ 1, 2 ], r.find( { a: { $gt: 0 } } ).sort( { a: 1 } ).hint( { a: 1 } ) );
    checkResults( [ 2, 1 ], r.find( { a: { $gt: 0 } } ).sort( { a: -1 } ).hint( { a: 1 } ) );
    checkResults( [ 1, 2 ], r.find( { a: { $gte: 1 } } ).sort( { a: 1 } ).hint( { a: 1 } ) );
    checkResults( [ 2, 1 ], r.find( { a: { $gte: 1 } } ).sort( { a: -1 } ).hint( { a: 1 } ) );
    
    checkResults( [ 0, 1 ], r.find( { a: { $lt: 2 } } ).sort( { a: 1 } ).hint( { a: 1 } ) );
    checkResults( [ 1, 0 ], r.find( { a: { $lt: 2 } } ).sort( { a: -1 } ).hint( { a: 1 } ) );
    checkResults( [ 0, 1 ], r.find( { a: { $lte: 1 } } ).sort( { a: 1 } ).hint( { a: 1 } ) );
    checkResults( [ 1, 0 ], r.find( { a: { $lte: 1 } } ).sort( { a: -1 } ).hint( { a: 1 } ) );
}

testConstrainedFindWithOrdering( db );
