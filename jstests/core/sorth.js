// Tests for the $in/sort/limit optimization combined with inequality bounds.  SERVER-5777


t = db.jstests_sorth;
t.drop();

/** Assert that the 'a' and 'b' fields of the documents match. */
function assertMatch( expectedMatch, match ) {
    if (undefined !== expectedMatch.a) {
        assert.eq( expectedMatch.a, match.a );
    }
    if (undefined !== expectedMatch.b) {
        assert.eq( expectedMatch.b, match.b );
    }
}

/** Assert an expected document or array of documents matches the 'matches' array. */
function assertMatches( expectedMatches, matches ) {
    if ( expectedMatches.length == null ) {
        assertMatch( expectedMatches, matches[ 0 ] );
    }
    for( i = 0; i < expectedMatches.length; ++i ) {
        assertMatch( expectedMatches[ i ], matches[ i ] );
    }
}

/** Generate a cursor using global parameters. */
function find( query ) {
    return t.find( query ).sort( _sort ).limit( _limit ).hint( _hint );
}

/** Check the expected matches for a query. */
function checkMatches( expectedMatch, query ) {
    result = find( query ).toArray();
    assertMatches( expectedMatch, result );
    explain = find( query ).explain();
    assert.eq( expectedMatch.length || 1, explain.n );
}

/** Reset data, index, and _sort and _hint globals. */
function reset( sort, index ) {
    t.drop();
    t.save( { a:1, b:1 } );
    t.save( { a:1, b:2 } );
    t.save( { a:1, b:3 } );
    t.save( { a:2, b:0 } );
    t.save( { a:2, b:3 } );
    t.save( { a:2, b:5 } );
    t.ensureIndex( index );
    _sort = sort;
    _hint = index;
}

function checkForwardDirection( sort, index ) {
    reset( sort, index );

    _limit = -1;

    // Lower bound checks.
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:0 } } );
    checkMatches( { a:1, b:1 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:0 } } );
    checkMatches( { a:1, b:1 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:1 } } );
    checkMatches( { a:1, b:2 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:1 } } );
    checkMatches( { a:1, b:2 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:2 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:2 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:3 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:3 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:4 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:4 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:5 } } );

    // Upper bound checks.
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:0 } } );
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:1 } } );
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:1 } } );
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3 } } );

    // Lower and upper bounds checks.
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:0, $lte:0 } } );
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:0, $lt:1 } } );
    checkMatches( { a:2, b:0 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:0, $lte:1 } } );
    checkMatches( { a:1, b:1 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:0, $lte:1 } } );
    checkMatches( { a:1, b:2 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:2, $lt:3 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:2.5, $lte:3 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:2.5, $lte:3 } } );

    // Limit is -2.
    _limit = -2;
    checkMatches( [ { a:2, b:0 }, { a:1, b:1 } ],
                  { a:{ $in:[ 1, 2 ] }, b:{ $gte:0 } } );
    // We omit 'a' here because it's not defined whether or not we will see
    // {a:2, b:3} or {a:1, b:3} first as our sort is over 'b'.
    checkMatches( [ { a:1, b:2 }, { b:3 } ],
                  { a:{ $in:[ 1, 2 ] }, b:{ $gt:1 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gt:4 } } );

    // With an additional document between the $in values.
    t.save( { a:1.5, b:3 } );
    checkMatches( [ { a:2, b:0 }, { a:1, b:1 } ],
                  { a:{ $in:[ 1, 2 ] }, b:{ $gte:0 } } );
}

// Basic test with an index suffix order.
checkForwardDirection( { b:1 }, { a:1, b:1 } );
// With an additonal index field.
checkForwardDirection( { b:1 }, { a:1, b:1, c:1 } );
// With an additonal reverse direction index field.
checkForwardDirection( { b:1 }, { a:1, b:1, c:-1 } );
// With an additonal ordered index field.
checkForwardDirection( { b:1, c:1 }, { a:1, b:1, c:1 } );
// With an additonal reverse direction ordered index field.
checkForwardDirection( { b:1, c:-1 }, { a:1, b:1, c:-1 } );

function checkReverseDirection( sort, index ) {
    reset( sort, index );
    _limit = -1;

    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:0 } } );
    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $gte:5 } } );

    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:5 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:5 } } );
    checkMatches( { a:1, b:2 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3.1 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3.5 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:3 } } );

    checkMatches( { a:2, b:5 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:5, $gte:5 } } );
    checkMatches( { a:1, b:1 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:2, $gte:1 } } );
    checkMatches( { a:1, b:2 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3, $gt:1 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lt:3.5, $gte:3 } } );
    checkMatches( { a:1, b:3 }, { a:{ $in:[ 1, 2 ] }, b:{ $lte:3, $gt:0 } } );
}

// With a descending order index.
checkReverseDirection( { b:-1 }, { a:1, b:-1 } );
checkReverseDirection( { b:-1 }, { a:1, b:-1, c:1 } );
checkReverseDirection( { b:-1 }, { a:1, b:-1, c:-1 } );
checkReverseDirection( { b:-1, c:1 }, { a:1, b:-1, c:1 } );
checkReverseDirection( { b:-1, c:-1 }, { a:1, b:-1, c:-1 } );
