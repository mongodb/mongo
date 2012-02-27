// Basic validation of explain output fields

if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode SERVER-5034

t = db.jstests_explain4;
t.drop();

function checkField( name, value ) {
    assert( explain.hasOwnProperty( name ) );
    if ( value != null ) {
        assert.eq( value, explain[ name ], name );
    }
}

function checkPlanFields( explain, matches, n ) {
    checkField( "cursor", "BasicCursor" );
    checkField( "n", n );
    checkField( "nscannedObjects", matches );
    checkField( "nscanned", matches );    
    checkField( "indexBounds", {} );
}

function checkFields( matches, sort, limit ) {
    it = t.find();
    if ( sort ) {
        it.sort({a:1});
    }
    if ( limit ) {
        it.limit( limit );
    }
    explain = it.explain( true );
//    printjson( explain );
    checkPlanFields( explain, matches, matches > 0 ? 1 : 0 );
    checkField( "scanAndOrder", sort );
    checkField( "millis" );
    checkField( "nYields" );
    checkField( "nChunkSkips", 0 );
    checkField( "isMultiKey", false );
    checkField( "indexOnly", false );
    checkField( "server" );
    checkField( "allPlans" );
    explain.allPlans.forEach( function( x ) { checkPlanFields( x, matches ); } );
}

checkFields( 0, false );
checkFields( 0, true );

t.save( {} );
checkFields( 1, false );
checkFields( 1, true );

t.save( {} );
checkFields( 1, false, 1 );
checkFields( 2, true, 1 );

}