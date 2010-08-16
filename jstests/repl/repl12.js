// SERVER-1626

function debug( x ) {
//    printjson( x );
}

rt = new ReplTest( "repl12tests" );

m = rt.start( true );

a = "a"
for( i = 0; i < 3; ++i ) {
    m.getDB( a ).c.save( {} );
    a += "a";
}
m.getDB( a ).getLastError();

s = rt.start( false );
assert.soon( function() { var c = s.getDBNames().length; debug( "count: " + c ); return c == 3; } );
