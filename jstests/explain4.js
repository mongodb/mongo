// Basic validation of explain output fields.

t = db.jstests_explain4;
t.drop();

function checkField( explain, name, value ) {
    assert( explain.hasOwnProperty( name ) );
    if ( value != null ) {
        assert.eq( value, explain[ name ], name );
        // Check that the value is of the expected type.  SERVER-5288
        assert.eq( typeof( value ), typeof( explain[ name ] ), 'type ' + name );
    }
}

function checkNonCursorPlanFields( explain, matches, n ) {
    checkField( explain, "n", n );
    checkField( explain, "nscannedObjects", matches );
    checkField( explain, "nscanned", matches );    
}

function checkPlanFields( explain, matches, n ) {
    checkField( explain, "cursor", "BasicCursor" );
    checkField( explain, "indexBounds", {} );    
    checkNonCursorPlanFields( explain, matches, n );
}

function checkFields( matches, sort, limit ) {
    cursor = t.find();
    if ( sort ) {
        cursor.sort({a:1});
    }
    if ( limit ) {
        cursor.limit( limit );
    }
    explain = cursor.explain( true );
//    printjson( explain );
    checkPlanFields( explain, matches, matches > 0 ? 1 : 0 );
    checkField( explain, "scanAndOrder", sort );
    checkField( explain, "millis" );
    checkField( explain, "nYields" );
    checkField( explain, "nChunkSkips", 0 );
    checkField( explain, "isMultiKey", false );
    checkField( explain, "indexOnly", false );
    checkField( explain, "server" );
    checkField( explain, "allPlans" );
    explain.allPlans.forEach( function( x ) { checkPlanFields( x, matches, matches ); } );
}

checkFields( 0, false );
checkFields( 0, true );

t.save( {} );
checkFields( 1, false );
checkFields( 1, true );

t.save( {} );
checkFields( 1, false, 1 );
checkFields( 2, true, 1 );

// Check basic fields with multiple clauses.
t.save( { _id:0 } );
explain = t.find( { $or:[ { _id:0 }, { _id:1 } ] } ).explain( true );
checkNonCursorPlanFields( explain, 1, 1 );
