// test arbitration

var baseName = "jstests_pair3test";

ismaster = function( n ) {
    var im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
    print( "ismaster: " + tojson( im ) );
    assert( im, "command ismaster failed" );
    return im.ismaster;
}

// bring up node connections before arbiter connections so that arb can forward to node when expected
connect = function() {
    if ( lp == null ) {
        lp = startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    }
    if ( rp == null ) {
        rp = startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
    }
    if ( al == null ) {
        al = startMongoProgram( "mongobridge", "--port", alPort, "--dest", "localhost:" + aPort );
    }
    if ( ar == null ) {
        ar = startMongoProgram( "mongobridge", "--port", arPort, "--dest", "localhost:" + aPort );
    }
}

disconnectNode = function( mongo ) {
    if ( lp ) {
        stopMongoProgram( lpPort );
        lp = null;
    }
    if ( rp ) {
        stopMongoProgram( rpPort );
        rp = null;
    }
    if ( mongo.host.match( new RegExp( "^127.0.0.1:" + lPort + "$" ) ) ) {
        stopMongoProgram( alPort );
        al = null;
    } else if ( mongo.host.match( new RegExp( "^127.0.0.1:" + rPort + "$" ) ) ) {
        stopMongoProgram( arPort );
        ar = null;
    } else {
        assert( false, "don't know how to disconnect node: " + mongo );
    }
}

doTest1 = function() {
    al = ar = lp = rp = null;
    ports = allocatePorts( 7 );
    aPort = ports[ 0 ];
    alPort = ports[ 1 ];
    arPort = ports[ 2 ];
    lPort = ports[ 3 ];
    lpPort = ports[ 4 ];
    rPort = ports[ 5 ];
    rpPort = ports[ 6 ];
    
    connect();
    
    a = new MongodRunner( aPort, "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( lPort, "/data/db/" + baseName + "-left", "127.0.0.1:" + rpPort, "127.0.0.1:" + alPort );
    r = new MongodRunner( rPort, "/data/db/" + baseName + "-right", "127.0.0.1:" + lpPort, "127.0.0.1:" + arPort );

    pair = new ReplPair( l, r, a );

    // normal startup
    pair.start();
    pair.waitForSteadyState();
    
    // disconnect slave
    disconnectNode( pair.slave() );
    pair.waitForSteadyState( [ 1, -3 ], pair.master().host );
    
    // disconnect master
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ -3, -3 ] );
    
    // reconnect
    connect();
    pair.waitForSteadyState();

    // disconnect master
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ 1, -3 ], pair.slave().host, true );
    
    // disconnect new master
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ -3, -3 ] );
    
    // reconnect
    connect();
    pair.waitForSteadyState();
    
    // disconnect slave
    disconnectNode( pair.slave() );
    pair.waitForSteadyState( [ 1, -3 ], pair.master().host );
    
    // reconnect slave
    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.master().host );
    
    // disconnect master
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ 1, -3 ], pair.slave().host, true );
    
    // reconnect old master
    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.master().host );

    ports.forEach( function( x ) { stopMongoProgram( x ); } );
}

// this time don't start connected
doTest2 = function() {
    ports = allocatePorts( 7 );
    aPort = ports[ 0 ];
    alPort = ports[ 1 ];
    arPort = ports[ 2 ];
    lPort = ports[ 3 ];
    lpPort = ports[ 4 ];
    rPort = ports[ 5 ];
    rpPort = ports[ 6 ];
    
    a = startMongod( "--port", aPort, "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", lPort, "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + rpPort, "127.0.0.1:" + alPort, "--oplogSize", "1", "--nohttpinterface" );
    r = startMongod( "--port", rPort, "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + lpPort, "127.0.0.1:" + arPort, "--oplogSize", "1", "--nohttpinterface" );

    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == -1 || lm == -3, "lm value invalid" );
                assert( rm == -1 || rm == -3, "rm value invalid" );
                
                return ( lm == -3 && rm == -3 );
                } );
    
    startMongoProgram( "mongobridge", "--port", arPort, "--dest", "localhost:" + aPort );

    // there hasn't been an initial sync, no no node will become master
    
    for( i = 0; i < 10; ++i ) {
        assert( ismaster( l ) == -3 && ismaster( r ) == -3 );
        sleep( 500 );
    }

    stopMongoProgram( arPort );

    startMongoProgram( "mongobridge", "--port", alPort, "--dest", "localhost:" + aPort );

    for( i = 0; i < 10; ++i ) {
        assert( ismaster( l ) == -3 && ismaster( r ) == -3 );
        sleep( 500 );
    }    

    stopMongoProgram( alPort );
    
    // connect l and r without a
    
    startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 0 || lm == -3, "lm value invalid" );
                assert( rm == 1 || rm == -3 || rm == 0, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );    
}

// recover from master - master setup
doTest3 = function() {
    al = ar = lp = rp = null;
    ports = allocatePorts( 7 );
    aPort = ports[ 0 ];
    alPort = ports[ 1 ];
    arPort = ports[ 2 ];
    lPort = ports[ 3 ];
    lpPort = ports[ 4 ];
    rPort = ports[ 5 ];
    rpPort = ports[ 6 ];
    
    connect();
    
    a = startMongod( "--port", aPort, "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", lPort, "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + rpPort, "127.0.0.1:" + alPort, "--oplogSize", "1", "--nohttpinterface" );
    r = startMongod( "--port", rPort, "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + lpPort, "127.0.0.1:" + arPort, "--oplogSize", "1", "--nohttpinterface" );

    // start normally
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == -1 || lm == 0, "lm value invalid" );
                assert( rm == -1 || rm == 0 || rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    stopMongoProgram( lpPort );
    stopMongoProgram( rpPort );
    
    // now each can only talk to arbiter
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 1 && rm == 1 );
                } );
    
    startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
    
    // recover
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );

    ports.forEach( function( x ) { stopMongoProgram( x ); } );
}

doTest1();
doTest2();
doTest3();
