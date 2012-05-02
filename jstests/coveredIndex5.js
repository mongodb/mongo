// Test use of covered indexes when there are multiple candidate indexes.

t = db.jstests_coveredIndex5;
t.drop();

t.ensureIndex( { a:1, b:1 } );
t.ensureIndex( { a:1, c:1 } );

function checkFields( query, projection ) {
    t.ensureIndex( { z:1 } ); // clear query patterns
    t.dropIndex( { z:1 } );

    results = t.find( query, projection ).toArray();

    expectedFields = [];
    for ( k in projection ) {
        if ( k != '_id' ) {
            expectedFields.push( k );
        }
    }

    vals = [];
    for ( i in results ) {
        r = results[ i ];
        assert.eq( 0, r.a );
        assert.eq( expectedFields, Object.keySet( r ) );
        for ( k in projection ) {
            if ( k != '_id' && k != 'a' ) {
                vals.push( r[ k ] );
            }
        }
    }

    if ( vals.length != 0 ) {
        vals.sort();
        assert.eq( [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 ], vals );
    }
}

function checkCursorCovered( cursor, covered, count, query, projection ) {
    checkFields( query, projection );
    explain = t.find( query, projection ).explain( true );
    assert.eq( cursor, explain.cursor );
    assert.eq( covered, explain.indexOnly );
    assert.eq( count, explain.n );
}

for( i = 0; i < 10; ++i ) {
    t.save( { a:0, b:i, c:9-i } );
}

checkCursorCovered( 'BtreeCursor a_1_b_1', true, 10, { a:0 }, { _id:0, a:1 } );
checkCursorCovered( 'BtreeCursor a_1_b_1', true, 10, { a:0, d:null }, { _id:0, a:1 } );
checkCursorCovered( 'BtreeCursor a_1_b_1', true, 10, { a:0, d:null }, { _id:0, a:1, b:1 } );

// Covered index on a,c not preferentially selected.
checkCursorCovered( 'BtreeCursor a_1_b_1', false, 10, { a:0, d:null }, { _id:0, a:1, c:1 } );

t.save( { a:0, c:[ 1, 2 ] } );
t.save( { a:1 } );
checkCursorCovered( 'BtreeCursor a_1_b_1', true, 11, { a:0, d:null }, { _id:0, a:1 } );

t.save( { a:0, b:[ 1, 2 ] } );
t.save( { a:1 } );
checkCursorCovered( 'BtreeCursor a_1_b_1', false, 12, { a:0, d:null }, { _id:0, a:1 } );

