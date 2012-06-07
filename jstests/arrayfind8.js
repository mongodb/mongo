// Index bounds and matching behavior for $elemMatch applied to a top level element.
// SERVER-1264
// SERVER-4180

t = db.jstests_arrayfind8;
t.drop();

function debug( x ) {
    if ( debuggingEnabled = false ) {
        printjson( x );
    }
}

/** Set index state for the test. */
function setIndexKey( key ) {
    indexKey = key;
    indexSpec = {};
    indexSpec[ key ] = 1;
}

setIndexKey( 'a' );

function indexBounds( query ) {
    debug( query );
    debug( t.find( query ).hint( indexSpec ).explain() );
    return t.find( query ).hint( indexSpec ).explain().indexBounds[ indexKey ];
}

/** Check index bounds for a query. */
function assertBounds( expectedBounds, query, context ) {
    bounds = indexBounds( query );
    debug( bounds );
    assert.eq( expectedBounds, bounds, 'unexpected bounds in ' + context );
}

/** Check that the query results match the documents in the 'expected' array. */
function assertResults( expected, query, context ) {
    debug( query );
    assert.eq( expected.length, t.count( query ), 'unexpected count in ' + context );
    results = t.find( query ).toArray();
    for( i in results ) {
        found = false;
        for( j in expected ) {
            if ( friendlyEqual( expected[ j ], results[ i ].a ) ) {
                found = true;
            }
        }
        assert( found, 'unexpected result ' + results[ i ] + ' in ' + context );
    }
}

/**
 * Check matching for different query types.
 * @param bothMatch - document matched by both standardQuery and elemMatchQuery
 * @param elemMatch - document matched by elemMatchQuery but not standardQuery
 * @param notElemMatch - document matched by standardQuery but not elemMatchQuery
 */
function checkMatch( bothMatch, elemMatch, nonElemMatch, standardQuery, elemMatchQuery, context ) {

    function mayPush( arr, elt ) {
        if ( elt ) {
            arr.push( elt );
        }
    }

    expectedStandardQueryResults = [];
    mayPush( expectedStandardQueryResults, bothMatch );
    mayPush( expectedStandardQueryResults, nonElemMatch );
    assertResults( expectedStandardQueryResults, standardQuery, context + ' standard query' );

    expectedElemMatchQueryResults = [];
    mayPush( expectedElemMatchQueryResults, bothMatch );
    mayPush( expectedElemMatchQueryResults, elemMatch );
    assertResults( expectedElemMatchQueryResults, elemMatchQuery, context + ' elemMatch query' );
}

/**
 * Check matching and index bounds for different query types.
 * @param subQuery - part of a query, to be provided as is for a standard query and within a
 *     $elemMatch clause for a $elemMatch query
 * @param singleKeyBounds - expected single key index bounds for the elem match query generated from
 *     @param subQuery
 * @param bothMatch - document matched by both standardQuery and elemMatchQuery
 * @param elemMatch - document matched by elemMatchQuery but not standardQuery
 * @param notElemMatch - document matched by standardQuery but not elemMatchQuery
 * @param additionalConstraints - additional query parameters not generated from @param subQuery
 * @param multiKeyBounds - expected multi key index bounds for the elem match query generated from
 *     @param subQuery.  If not provided, singleKeyBounds will be expected.
 */
function checkBoundsAndMatch( subQuery, singleKeyBounds, bothMatch, elemMatch,
                              nonElemMatch, additionalConstraints, multiKeyBounds ) {
    t.drop();
    multiKeyBounds = multiKeyBounds || singleKeyBounds;
    additionalConstraints = additionalConstraints || {};
    
    // Construct standard and elemMatch queries from subQuery.
    firstSubQueryKey = Object.keySet( subQuery )[ 0 ];
    if ( firstSubQueryKey[ 0 ] == '$' ) {
        standardQuery = { $and:[ { a:subQuery }, additionalConstraints ] };
    }
    else {
        // If the subQuery contains a field rather than operators, append to the 'a' field.
        modifiedSubQuery = {};
        modifiedSubQuery[ 'a.' + firstSubQueryKey ] = subQuery[ firstSubQueryKey ];
        standardQuery = { $and:[ modifiedSubQuery, additionalConstraints ] };
    }
    elemMatchQuery = { $and:[ { a:{ $elemMatch:subQuery } }, additionalConstraints ] };
    debug( elemMatchQuery );

    function maySave( aValue ) {
        if ( aValue ) {
            debug( { a:aValue } );
            t.save( { a:aValue } );
        }
    }

    // Save all documents and check matching without indexes.
    maySave( bothMatch );
    maySave( elemMatch );
    maySave( nonElemMatch );

    checkMatch( bothMatch, elemMatch, nonElemMatch, standardQuery, elemMatchQuery, 'unindexed' );

    // Check matching and index bounds for a single key index.
    
    t.drop();
    maySave( bothMatch );
    maySave( elemMatch );
    // The nonElemMatch document is not tested here, as it will often make the index multikey.
    t.ensureIndex( indexSpec );
    checkMatch( bothMatch, elemMatch, null, standardQuery, elemMatchQuery, 'single key index' );
    assertBounds( singleKeyBounds, elemMatchQuery, 'single key index' );

    // Check matching and index bounds for a multikey index.
    
    // Now the nonElemMatch document is tested.
    maySave( nonElemMatch );
    // Force the index to be multikey.
    t.save( { a:[ -1, -2 ] } );
    t.save( { a:{ b:[ -1, -2 ] } } );
    checkMatch( bothMatch, elemMatch, nonElemMatch, standardQuery, elemMatchQuery,
                'multikey index' );
    assertBounds( multiKeyBounds, elemMatchQuery, 'multikey index' );
}

maxNumber = 1.7976931348623157e+308;

// Basic test.
checkBoundsAndMatch( { $gt:4 }, [[ 4, maxNumber ]], [ 5 ] );

// Multiple constraints within a $elemMatch clause.
checkBoundsAndMatch( { $gt:4, $lt:6 }, [[ 4, 6 ]], [ 5 ], null, [ 3, 7 ] );
checkBoundsAndMatch( { $gt:4, $not:{ $gte:6 } }, [[ 4, 6 ]], [ 5 ] );
checkBoundsAndMatch( { $gt:4, $not:{ $ne:6 } }, [[ 6, 6 ]], [ 6 ] );
checkBoundsAndMatch( { $gte:5, $lte:5 }, [[ 5, 5 ]], [ 5 ], null, [ 4, 6 ] );
checkBoundsAndMatch( { $in:[ 4, 6 ], $gt:5 }, [[ 6, 6 ]], [ 6 ], null, [ 4, 7 ] );
checkBoundsAndMatch( { $regex:'^a' }, [[ 'a', 'b' ], [ /^a/, /^a/ ]], [ 'a' ] );
checkBoundsAndMatch( { $regex:'^a', $in:['b'] }, undefined ); // ?? undefined

// Some constraints within a $elemMatch clause and other constraints outside of it.
checkBoundsAndMatch( { $gt:4 }, [[ 4, 6 ]], [ 5 ], null, null, { a:{ $lt:6 } },
                     [[ 4, maxNumber ]] );
checkBoundsAndMatch( { $gte:5 }, [[ 5, 5 ]], [ 5 ], null, null, { a:{ $lte:5 } },
                     [[ 5, maxNumber ]] );
checkBoundsAndMatch( { $in:[ 4, 6 ] }, [[ 6, 6 ]], [ 6 ], null, null, { a:{ $gt:5 } },
                     [[ 4, 4 ], [ 6, 6 ]] );

// Constraints in different $elemMatch clauses.
checkBoundsAndMatch( { $gt:4 }, [[ 4, 6 ]], [ 5 ], null, null, { a:{ $elemMatch:{ $lt:6 } } },
                     [[ 4, maxNumber ]] );
checkBoundsAndMatch( { $gt:4 }, [[ 4, maxNumber ]], [ 3, 7 ], null, null,
                     { a:{ $elemMatch:{ $lt:6 } } }, [[ 4, maxNumber ]] );
checkBoundsAndMatch( { $gte:5 }, [[ 5, 5 ]], [ 5 ], null, null, { a:{ $elemMatch:{ $lte:5 } } },
                     [[ 5, maxNumber ]] );
checkBoundsAndMatch( { $in:[ 4, 6 ] }, [[ 6, 6 ]], [ 6 ], null, null,
                     { a:{ $elemMatch:{ $gt:5 } } }, [[ 4, 4 ], [ 6, 6 ]] );

// TODO SERVER-1264
if ( 0 ) {
checkBoundsAndMatch( { $elemMatch:{ $in:[ 5 ] } }, [[ {$minElement:1}, {$maxElement:1} ]], null,
                     [[ 5 ]], [ 5 ], null, [[ {$minElement:1}, {$maxElement:1} ]] );
}

// Index bounds are not computed for $elemMatch nested within a $elemMatch applied to a top level
// element (descriptive, not normative test).  The reason is that { a:[ [ { b:1 } ] ] } matches a
// query as in the example, but double nested arrays are not indexed as multikeys.
setIndexKey( 'a.b' );
checkBoundsAndMatch( { $elemMatch:{ b:{ $gte:1, $lte:1 } } },
                     [[ {$minElement:1}, {$maxElement:1} ]], null, [[ { b:1 } ]], [ { b:1 } ],
                     null, [[ {$minElement:1}, {$maxElement:1} ]] );
checkBoundsAndMatch( { $elemMatch:{ b:{ $gte:1, $lte:1 } } },
                     [[ {$minElement:1}, {$maxElement:1} ]], null, [[ { b:[ 0, 2 ] } ]],
                     [ { b:[ 0, 2 ] } ], null, [[ {$minElement:1}, {$maxElement:1} ]] );

// Constraints for a top level (SERVER-1264 style) $elemMatch nested within a non top level
// $elemMatch.
checkBoundsAndMatch( { b:{ $elemMatch:{ $gte:1, $lte:1 } } }, [[ 1, 1 ]], [ { b:[ 1 ] } ] );
checkBoundsAndMatch( { b:{ $elemMatch:{ $gte:1, $lte:4 } } }, [[ 1, 4 ]], [ { b:[ 1 ] } ] );
checkBoundsAndMatch( { b:{ $elemMatch:{ $gte:1, $lte:4 } } }, [[ 2, 2 ]], [ { b:[ 2 ] } ], null,
                     null, { 'a.b':{ $in:[ 2, 5 ] } }, [[ 1, 4 ]] );
checkBoundsAndMatch( { b:{ $elemMatch:{ $in:[ 1, 2 ] }, $in:[ 2, 3 ] } }, [[ 2, 2 ]],
                     [ { b:[ 2 ] } ], null, [ { b:[ 1 ] }, { b:[ 3 ] } ], null,
                     [[ 1, 1 ], [ 2, 2 ]] );
