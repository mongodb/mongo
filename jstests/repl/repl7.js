// Test persistence of list of dbs to add.

var baseName = "jstests_repl7test";

doTest = function( signal ) {
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );

    for( n = "a"; n != "aaaaa"; n += "a" ) {
        m.getDB( n ).a.save( {x:1} );
    }

    s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    
    assert.soon( function() {
                return -1 != s.getDBNames().indexOf( "aa" );
                } );
    
    stopMongod( 27019, signal );
    
    sleep( 4000 );
    
    s = startMongoProgram( "mongod", "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );    
    
    assert.soon( function() {
                for( n = "a"; n != "aaaaa"; n += "a" ) {
                    if ( -1 == s.getDBNames().indexOf( n ) )
                        return false;                    
                }
                return true;
                } );

    for( n = "a"; n != "aaaaa"; n += "a" ) {
        assert.eq( 1, m.getDB( n ).a.find().count() );
    }    
    
    stopMongod( 27019 );
    stopMongod( 27018 );
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
