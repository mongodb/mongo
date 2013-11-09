// Nested $elemMatch clauses.  SERVER-5741

t = db.jstests_arrayfind7;
t.drop();

t.save( { a:[ { b:[ { c:1, d:2 } ] } ] } );

function checkElemMatchMatches() {
    assert.eq( 1, t.count( { a:{ $elemMatch:{ b:{ $elemMatch:{ c:1, d:2 } } } } } ) );
}

// The document is matched using nested $elemMatch expressions, with and without an index.
checkElemMatchMatches();
t.ensureIndex( { 'a.b.c':1 } );
checkElemMatchMatches();

function checkIndexCharacterBasedBounds( index, document, query, singleKeyBounds, multiKeyBounds ) {
    // The document is matched without an index, and with single and multi key indexes.
    t.drop();
    t.save( document );
    assert.eq( 1, t.count( query ) );
    t.ensureIndex( index );
    assert.eq( 1, t.count( query ) );
    t.save( { a:{ b:{ c:[ 10, 11 ] } } } ); // Make the index multikey.
    assert.eq( 1, t.count( query ) );

    // The single and multi key index bounds are as expected.
    t.drop();
    t.ensureIndex( index );
    assert.eq( singleKeyBounds, t.find( query ).explain().indexBounds[ 'a.b.c' ] );
    t.save( { a:{ b:{ c:[ 10, 11 ] } } } );
    assert.eq( multiKeyBounds, t.find( query ).explain().indexBounds[ 'a.b.c' ] );
}

// QUERY_MIGRATION: all bounds outputted by the new system are supersets of the bounds expected
// here.  So we're not wrong, we're just slow.

// Two constraints within a nested $elemMatch expression.
checkIndexCharacterBasedBounds( { 'a.b.c':1 },
                               { a:[ { b:[ { c:1 } ] } ] },
                               { a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $lte:1 } } } } } },
                               [ [ 1, 1 ] ],
                               [ [ 1, Infinity ] ] );

var QUERY_MIGRATION_COMPLETE = false;

if (QUERY_MIGRATION_COMPLETE) {
    // Two constraints within a nested $elemMatch expression, one of which contains the other.
    //
    // QUERY_MIGRATION: We should be smarter about which predicate we choose to use as our bounds
    // when we have many available for one field of a multikey index.
    checkIndexCharacterBasedBounds( { 'a.b.c':1 },
                                   { a:[ { b:[ { c:2 } ] } ] },
                                   { a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1, $in:[2] } } } } } },
                                   [ [ 2, 2 ] ],
                                   [ [ 2, 2 ] ] );

    // Two nested $elemMatch expressions.
    //
    // QUERY_MIGRATION: there exists an elemmatch that contains the elemmatches over d.e and b.c so we
    // can combine them in the single key case.  we don't do this though.
    //
    // the multikey case is ok.
    checkIndexCharacterBasedBounds( { 'a.d.e':1, 'a.b.c':1 },
                                   { a:[ { b:[ { c:1 } ], d:[ { e:1 } ] } ] },
                                   { a:{ $elemMatch:{ d:{ $elemMatch:{ e:{ $lte:1 } } },
                                                      b:{ $elemMatch:{ c:{ $gte:1 } } } } } },
                                   [ [ 1, Infinity ] ],
                                   [ [ { $minElement:1 }, { $maxElement:1 } ] ] );

    // A non $elemMatch expression and a nested $elemMatch expression.
    // 
    // QUERY_MIGRATION: for enumeration/compounding purposes, elemMatches should be treated as ANDs and
    // folded in.  we don't consider that we can compound-AND with it (single key).
    //
    // the multikey is ok.
    checkIndexCharacterBasedBounds( { 'a.x':1, 'a.b.c':1 },
                                   { a:[ { b:[ { c:1 } ], x:1 } ] },
                                   { 'a.x':1, a:{ $elemMatch:{ b:{ $elemMatch:{ c:{ $gte:1 } } } } } },
                                   [ [ 1, Infinity ] ],
                                   [ [ { $minElement:1 }, { $maxElement:1 } ] ] );
}

// $elemMatch is applied directly to a top level field.
checkIndexCharacterBasedBounds( { 'a.b.c':1 },
                               { a:[ { b:[ { c:[ 1 ] } ] } ] },
                               { a:{ $elemMatch:{ 'b.c':{ $elemMatch:{ $gte:1, $lte:1 } } } } },
                               [ [ 1, 1 ] ],
                               [ [ 1, 1 ] ] );
