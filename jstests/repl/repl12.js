// SERVER-1626
// check for initial sync of multiple db's

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
m.getDB(a).getLastError();

//print("\n\n\n DB NAMES MASTER:");
//printjson(m.getDBNames());

var z = 10500;
print("sleeping " + z + "ms");
sleep(z);

s = rt.start(false);

assert.soon( function() { var c = s.getDBNames().length; debug( "count: " + c ); return c == 3; } );

//printjson(s.getDBNames());
