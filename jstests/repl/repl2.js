// Test resync command

var baseName = "jstests_repl2test";

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "check count" );
                if ( -1 == s.getDBNames().indexOf( baseName ) )
                    return false;
                if ( -1 == s.getDB( baseName ).getCollectionNames().indexOf( "a" ) )
                    return false;
//                print( "count: " + s.getDB( baseName ).z.find().count() );
                return s.getDB( baseName ).a.find().count() == count; 
                } );    
}

doTest = function( signal ) {
    
    // spec small oplog to make slave get out of sync
    m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );
    s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    
    am = m.getDB( baseName ).a
    
    am.save( { _id: new ObjectId() } );
    soonCount( 1 );
    assert.eq( 0, s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );
    stopMongod( 27019, signal );
    sleep( 2000 );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i, b: big } );
    
    s = startMongoProgram( "mongod", "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    assert.soon( function() { return 1 == s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok; } );

    sleep( 10000 );
    soonCount( 1001 );
    as = s.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );
    
    assert.eq( 0, s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    stopMongod( 27018 );
    stopMongod( 27019 );
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
