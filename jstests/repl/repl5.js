// Test auto reclone after failed initial clone

var baseName = "jstests_repl5test";

soonCountAtLeast = function( db, coll, count ) {
    assert.soon( function() { 
//                print( "count: " + s.getDB( db )[ coll ].find().count() );
                return s.getDB( db )[ coll ].find().count() >= count; 
                } );    
}

doTest = function( signal ) {

    ports = allocatePorts( 2 );
    
    m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    ma = m.getDB( "a" ).a;
    for( i = 0; i < 10000; ++i )
        ma.save( { i:i } );
    
    s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    soonCountAtLeast( "a", "a", 1 );
    stopMongod( ports[ 1 ], signal );

    s = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    sleep( 1000 );
    soonCountAtLeast( "a", "a", 10000 );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
