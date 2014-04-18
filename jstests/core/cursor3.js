// Test inequality bounds combined with ordering for a single-field index.
// BUG 1079 (fixed)

testNum = 1;

function checkResults( expected, cursor , testNum ) {
    assert.eq( expected.length, cursor.count() , "testNum: " + testNum  + " A : " + tojson( cursor.toArray() ) + " " + tojson( cursor.explain() ) );
    for( i = 0; i < expected.length; ++i ) {
	assert.eq( expected[ i ], cursor[ i ][ "a" ] , "testNum: " + testNum + " B" );
    }
}

t = db.cursor3;
t.drop()

t.save( { a: 0 } );
t.save( { a: 1 } );
t.save( { a: 2 } );

t.ensureIndex( { a: 1 } );



checkResults( [ 1 ], t.find( { a: 1 } ).sort( { a: 1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 1 ], t.find( { a: 1 } ).sort( { a: -1 } ).hint( { a: 1 } ) , testNum++ )

checkResults( [ 1, 2 ], t.find( { a: { $gt: 0 } } ).sort( { a: 1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 2, 1 ], t.find( { a: { $gt: 0 } } ).sort( { a: -1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 1, 2 ], t.find( { a: { $gte: 1 } } ).sort( { a: 1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 2, 1 ], t.find( { a: { $gte: 1 } } ).sort( { a: -1 } ).hint( { a: 1 } ) , testNum++ )

checkResults( [ 0, 1 ], t.find( { a: { $lt: 2 } } ).sort( { a: 1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 1, 0 ], t.find( { a: { $lt: 2 } } ).sort( { a: -1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 0, 1 ], t.find( { a: { $lte: 1 } } ).sort( { a: 1 } ).hint( { a: 1 } ) , testNum++ )
checkResults( [ 1, 0 ], t.find( { a: { $lte: 1 } } ).sort( { a: -1 } ).hint( { a: 1 } ) , testNum++ )
