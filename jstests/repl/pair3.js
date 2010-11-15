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
        print("connecting lp");
        lp = startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    }
    if ( rp == null ) {
        print("connecting rp");
        rp = startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
    }
    if ( al == null ) {
        print("connecting al");
        al = startMongoProgram( "mongobridge", "--port", alPort, "--dest", "localhost:" + aPort );
    }
    if ( ar == null ) {
        print("connecting ar");
        ar = startMongoProgram( "mongobridge", "--port", arPort, "--dest", "localhost:" + aPort );
    }
}

disconnectNode = function( mongo ) {    
    if ( lp ) {
        print("disconnecting lp: "+lpPort);
        stopMongoProgram( lpPort );
        lp = null;
    }
    if ( rp ) {
        print("disconnecting rp: "+rpPort);
        stopMongoProgram( rpPort );
        rp = null;
    }
    if ( mongo.host.match( new RegExp( "^127.0.0.1:" + lPort + "$" ) ) ) {
        print("disconnecting al: "+alPort);
        stopMongoProgram( alPort );
        al = null;
    } else if ( mongo.host.match( new RegExp( "^127.0.0.1:" + rPort + "$" ) ) ) {
        print("disconnecting ar: "+arPort);
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

    print("normal startup");
    pair.start();
    pair.waitForSteadyState();
    
    print("disconnect slave");
    disconnectNode( pair.slave() );
    pair.waitForSteadyState( [ 1, -3 ], pair.master().host );
    
    print("disconnect master");
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ -3, -3 ] );
    
    print("reconnect");
    connect();
    pair.waitForSteadyState();

    print("disconnect master");
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ 1, -3 ], pair.slave().host, true );
    
    print("disconnect new master");
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ -3, -3 ] );
    
    print("reconnect");
    connect();
    pair.waitForSteadyState();
    
    print("disconnect slave");
    disconnectNode( pair.slave() );
    pair.waitForSteadyState( [ 1, -3 ], pair.master().host );
    
    print("reconnect slave");
    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.master().host );
    
    print("disconnect master");
    disconnectNode( pair.master() );
    pair.waitForSteadyState( [ 1, -3 ], pair.slave().host, true );
    
    print("reconnect old master");
    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.master().host );

    ports.forEach( function( x ) { stopMongoProgram( x ); } );
}

// this time don't start connected
doTest2 = function() {
    al = ar = lp = rp = null;
    ports = allocatePorts( 7 );
    aPort = ports[ 0 ];
    alPort = ports[ 1 ];
    arPort = ports[ 2 ];
    lPort = ports[ 3 ];
    lpPort = ports[ 4 ];
    rPort = ports[ 5 ];
    rpPort = ports[ 6 ];
    
    a = new MongodRunner( aPort, "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( lPort, "/data/db/" + baseName + "-left", "127.0.0.1:" + rpPort, "127.0.0.1:" + alPort );
    r = new MongodRunner( rPort, "/data/db/" + baseName + "-right", "127.0.0.1:" + lpPort, "127.0.0.1:" + arPort );
    
    pair = new ReplPair( l, r, a );
    pair.start();
    pair.waitForSteadyState( [ -3, -3 ] );
    
    startMongoProgram( "mongobridge", "--port", arPort, "--dest", "localhost:" + aPort );

    // there hasn't been an initial sync, no no node will become master
    
    for( i = 0; i < 10; ++i ) {
        assert( pair.isMaster( pair.right() ) == -3 && pair.isMaster( pair.left() ) == -3 );
        sleep( 500 );
    }

    stopMongoProgram( arPort );

    startMongoProgram( "mongobridge", "--port", alPort, "--dest", "localhost:" + aPort );

    for( i = 0; i < 10; ++i ) {
        assert( pair.isMaster( pair.right() ) == -3 && pair.isMaster( pair.left() ) == -3 );
        sleep( 500 );
    }    

    stopMongoProgram( alPort );
    
    // connect l and r without a
    
    startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );

    pair.waitForSteadyState( [ 1, 0 ] );
    
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
    
    a = new MongodRunner( aPort, "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( lPort, "/data/db/" + baseName + "-left", "127.0.0.1:" + rpPort, "127.0.0.1:" + alPort );
    r = new MongodRunner( rPort, "/data/db/" + baseName + "-right", "127.0.0.1:" + lpPort, "127.0.0.1:" + arPort );
    
    pair = new ReplPair( l, r, a );
    pair.start();
    pair.waitForSteadyState();

    // now can only talk to arbiter
    stopMongoProgram( lpPort );
    stopMongoProgram( rpPort );
    pair.waitForSteadyState( [ 1, 1 ], null, true );
    
    // recover
    startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
    pair.waitForSteadyState( [ 1, 0 ], null, true );

    ports.forEach( function( x ) { stopMongoProgram( x ); } );
}

// check that initial sync is persistent
doTest4 = function( signal ) {
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
    pair.start();
    pair.waitForSteadyState();

    pair.killNode( pair.left(), signal );
    pair.killNode( pair.right(), signal );
    stopMongoProgram( rpPort );
    stopMongoProgram( lpPort );
    
    // now can only talk to arbiter
    pair.start( true );
    pair.waitForSteadyState( [ 1, 1 ], null, true );

    ports.forEach( function( x ) { stopMongoProgram( x ); } );
}

doTest1();
doTest2();
doTest3();
doTest4( 15 );
doTest4( 9 );
