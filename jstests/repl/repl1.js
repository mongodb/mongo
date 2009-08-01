// Test basic replication functionality

var baseName = "jstests_repl1test";

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "check count" );
//                print( "count: " + s.getDB( baseName ).z.find().count() );
                return s.getDB( baseName ).a.find().count() == count; 
                } );    
}

doTest = function( signal ) {

    ports = allocatePorts( 2 );
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    am = m.getDB( baseName ).a
    
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    soonCount( 1000 );
    as = s.getDB( baseName ).a    
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );

    stopMongod( ports[ 1 ], signal );
    
    for( i = 1000; i < 1010; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    s = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    soonCount( 1010 );
    as = s.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 1009 } ).count() );

    stopMongod( ports[ 0 ], signal );
    
    m = startMongoProgram( "mongod", "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    am = m.getDB( baseName ).a

    for( i = 1010; i < 1020; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    assert.soon( function() { return as.find().count() == 1020; } );
    assert.eq( 1, as.find( { i: 1019 } ).count() );

    ports.forEach( function( x ) { stopMongod( x ); } );
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
