// Test one master replicating to two slaves

var baseName = "jstests_repl6test";

soonCount = function( m, count ) {
    assert.soon( function() { 
                return m.getDB( baseName ).a.find().count() == count; 
                }, "expected count: " + count + " from : " + m );    
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    s1 = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave1", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    s2 = startMongod( "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-slave2", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    am = m.getDB( baseName ).a
    
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i } );
    
    soonCount( s1, 1000 );
    soonCount( s2, 1000 );

    check = function( as ) {
        assert.eq( 1, as.find( { i: 0 } ).count() );
        assert.eq( 1, as.find( { i: 999 } ).count() );        
    }
    
    as = s1.getDB( baseName ).a
    check( as );
    as = s2.getDB( baseName ).a    
    check( as );    

    stopMongod( ports[ 1 ], signal );
    stopMongod( ports[ 2 ], signal );
    
    for( i = 1000; i < 1010; ++i )
        am.save( { _id: new ObjectId(), i: i } );
    
    s1 = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave1", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    soonCount( s1, 1010 );
    as = s1.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 1009 } ).count() );
    
    stopMongod( ports[ 0 ], signal );
    
    m = startMongoProgram( "mongod", "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    am = m.getDB( baseName ).a
    
    for( i = 1010; i < 1020; ++i )
        am.save( { _id: new ObjectId(), i: i } );
    
    soonCount( s1, 1020 );
    assert.eq( 1, as.find( { i: 1019 } ).count() );

    s2 = startMongoProgram( "mongod", "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-slave2", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    soonCount( s2, 1020 );
    as = s2.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 1009 } ).count() );
    assert.eq( 1, as.find( { i: 1019 } ).count() );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
