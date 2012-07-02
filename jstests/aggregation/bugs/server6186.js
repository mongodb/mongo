// $substr returns an empty string if the position argument is out of bounds.  SERVER-6186

t = db.jstests_aggregation_server6186;
t.drop();

t.save( {} );

function substr( string, pos, n ) {
    return t.aggregate( { $project:{ a:{ $substr:[ string, pos, n ] } } } ).result[ 0 ].a;
}

function expectedSubstr( string, pos, n ) {
    if ( pos < 0 ) {
        // A negative value is interpreted as a large unsigned int, and is expected to be out of
        // bounds.
        return "";
    }
    if ( n < 0 ) {
        // A negative value is interpreted as a large unsigned int, expected to exceed the length
        // of the string.  Passing the string length is functionally equivalent.
        n = string.length;
    }
    return string.substring( pos, pos + n );
}

function assertSubstr( string, pos, n ) {
    assert.eq( expectedSubstr( string, pos, n ), substr( string, pos, n ) );
}

function checkVariousSubstrings( string ) {
    for( pos = -2; pos < 5; ++pos ) {
        for( n = -2; n < 7; ++n ) {
            assertSubstr( string, pos, n );
        }
    }
}

checkVariousSubstrings( "abc" );
checkVariousSubstrings( "" );
