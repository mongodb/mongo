// Test resync after failed initial clone

var baseName = "jstests_repl5test";

soonCountAtLeast = function( db, coll, count ) {
    assert.soon( function() { 
                if ( -1 == s.getDBNames().indexOf( db ) )
                    return false;
                if ( -1 == s.getDB( db ).getCollectionNames().indexOf( coll ) )
                    return false;
//                print( "count: " + s.getDB( db )[ coll ].find().count() );
                return s.getDB( db )[ coll ].find().count() >= count; 
                } );    
}

doTest = function( signal ) {
    
    m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );
    
    ma = m.getDB( "a" ).a;
    for( i = 0; i < 10000; ++i )
        ma.save( { i:i } );
    
    s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    soonCountAtLeast( "a", "a", 1 );
    stopMongod( 27019, signal );

    s = startMongoProgram( "mongod", "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    sleep( 1000 );
    ma.save( { i:-1 } );

    ma.save( { i:-2 } );
    soonCountAtLeast( "a", "a", 10002 );

    stopMongod( 27018 );
    stopMongod( 27019 );
}

doTest( 15 ); // SIGTERM
sleep( 2000 );
doTest( 9 );  // SIGKILL
