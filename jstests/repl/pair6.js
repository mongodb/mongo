// pairing cases where oplogs run out of space

var baseName = "jstests_pair6test";

debug = function( p ) {
    print( p );
}

ismaster = function( n ) {
    var im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
    print( "ismaster: " + tojson( im ) );
    assert( im, "command ismaster failed" );
    return im.ismaster;
}

connect = function() {
    startMongoProgram( "mongobridge", "--port", lpPort, "--dest", "localhost:" + lPort );
    startMongoProgram( "mongobridge", "--port", rpPort, "--dest", "localhost:" + rPort );
}

disconnect = function() {
    stopMongoProgram( lpPort );
    stopMongoProgram( rpPort );
}

checkCount = function( m, c ) {
    m.setSlaveOk();
    assert.soon( function() {
                actual = m.getDB( baseName ).getCollection( baseName ).find().count();
                print( actual );
                return c == actual; },
                "expected count " + c + " for " + m );
}

resetSlave = function( s ) {
    s.setSlaveOk();
    assert.soon( function() {
                ret = s.getDB( "admin" ).runCommand( { "resync" : 1 } );
                //                printjson( ret );
                return 1 == ret.ok;
                } );    
}

big = new Array( 2000 ).toString();

doTest = function() {
    ports = allocatePorts( 5 );
    aPort = ports[ 0 ];
    lPort = ports[ 1 ];
    lpPort = ports[ 2 ];
    rPort = ports[ 3 ];
    rpPort = ports[ 4 ];
    
    // start normally
    connect();
    a = new MongodRunner( aPort, "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( lPort, "/data/db/" + baseName + "-left", "127.0.0.1:" + rpPort, "127.0.0.1:" + aPort );
    r = new MongodRunner( rPort, "/data/db/" + baseName + "-right", "127.0.0.1:" + lpPort, "127.0.0.1:" + aPort );
    pair = new ReplPair( l, r, a );
    pair.start();
    pair.waitForSteadyState();

    disconnect();    
    pair.waitForSteadyState( [ 1, 1 ], null, true );

    print( "test one" );
    
    // fill new slave oplog
    for( i = 0; i < 1000; ++i ) {
        pair.left().getDB( baseName ).getCollection( baseName ).save( {b:big} );
    }
    pair.left().getDB( baseName ).getCollection( baseName ).findOne();
    
    // write single to new master
    pair.right().getDB( baseName ).getCollection( baseName ).save( {} );
    
    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.right().host, true );

    resetSlave( pair.left() );
    
    checkCount( pair.left(), 1 );
    checkCount( pair.right(), 1 );
    
    pair.right().getDB( baseName ).getCollection( baseName ).remove( {} );
    checkCount( pair.left(), 0 );
    
    disconnect();    
    pair.waitForSteadyState( [ 1, 1 ], null, true );

    print( "test two" );
    
    // fill new master oplog
    for( i = 0; i < 1000; ++i ) {
        pair.right().getDB( baseName ).getCollection( baseName ).save( {b:big} );
    }

    pair.left().getDB( baseName ).getCollection( baseName ).save( {_id:"abcde"} );

    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.right().host, true );
    
    sleep( 15000 );
    
    resetSlave( pair.left() );
    
    checkCount( pair.left(), 1000 );
    checkCount( pair.right(), 1000 );
    assert.eq( 0, pair.left().getDB( baseName ).getCollection( baseName ).find( {_id:"abcde"} ).count() );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

doTest();