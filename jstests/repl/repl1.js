// Test basic replication functionality

var baseName = "jstests_repl1test";

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
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );
    s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    
    am = m.getDB( baseName ).a
    
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    soonCount( 1000 );
    as = s.getDB( baseName ).a    
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );

    stopMongod( 27019, signal );
    
    for( i = 1000; i < 1010; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    s = startMongoProgram( "mongod", "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018" );
    soonCount( 1010 );
    as = s.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 1009 } ).count() );

    stopMongod( 27018, signal );
    
    m = startMongoProgram( "mongod", "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );
    am = m.getDB( baseName ).a

    for( i = 1010; i < 1020; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    assert.soon( function() { return as.find().count() == 1020; } );
    assert.eq( 1, as.find( { i: 1019 } ).count() );
    
    stopMongod( 27018 );
    stopMongod( 27019 );
}

doTest( 15 ); // SIGTERM
sleep( 2000 );
doTest( 9 );  // SIGKILL
