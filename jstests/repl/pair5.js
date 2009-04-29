// writes to new master while making master-master logs consistent

var baseName = "jstests_pair5test";

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

write = function( m, n, id ) {
    if ( id ) {
        save = { _id:id, n:n };
    } else {
        save = { n:n };
    }
    m.getDB( baseName ).getCollection( baseName ).save( save );
}

checkCount = function( m, c ) {
    m.setSlaveOk();
    assert.soon( function() {
                actual = m.getDB( baseName ).getCollection( baseName ).find().count();
                print( actual );
                return c == actual; },
                "count failed for " + m );
}

doTest = function( nSlave, opIdMem ) {
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
    
    // now each can only talk to arbiter
    disconnect();    
    pair.waitForSteadyState( [ 1, 1 ], null, true );
    
    // left will become slave
    for( i = 0; i < nSlave; ++i ) {
        write( pair.left(), i, i );
    }    
    pair.left().getDB( baseName ).getCollection( baseName ).findOne();

    for( i = 10000; i < 15000; ++i ) {
        write( pair.right(), i, i );
    }    
    pair.right().getDB( baseName ).getCollection( baseName ).findOne();

    connect();
    pair.waitForSteadyState( [ 1, 0 ], pair.right().host, true );

    pair.master().getDB( baseName ).getCollection( baseName ).update( {_id:nSlave - 1}, {_id:nSlave - 1,n:-1}, true );
    assert.eq( -1, pair.master().getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n );
    checkCount( pair.master(), 5000 + nSlave );
    assert.eq( -1, pair.master().getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n );
    pair.slave().setSlaveOk();
    assert.soon( function() {
                n = pair.slave().getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n;
                print( n );
                return -1 == n;
                } );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

doTest( 5000, 100000000 );
doTest( 5000, 100 ); // force op id converstion to collection based storage
