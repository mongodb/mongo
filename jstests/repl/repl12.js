// SERVER-1626
// check for initial sync of multiple db's

function debug( x ) {
    print( "DEBUG:" + tojson( x ) );
}

rt = new ReplTest( "repl12tests" );

m = rt.start( true );

usedDBs = []

a = "a"
for( i = 0; i < 3; ++i ) {
    usedDBs.push( a )
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

function countHave(){
    var have = 0;
    for ( var i=0; i<usedDBs.length; i++ ){
        if ( s.getDB( usedDBs[i] ).c.findOne() )
            have++;
    }
    return have;
}

assert.soon( 
    function() { 
        try {
            var c = countHave();
            debug( "count: " + c );
            return c == 3;
        } catch (e) {
            printjson(e);
            return false;
        }
    }
);

//printjson(s.getDBNames());
