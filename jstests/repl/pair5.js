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
    a = startMongod( "--port", aPort, "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", lPort, "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + rpPort, "127.0.0.1:" + aPort, "--oplogSize", "1", "--opIdMem", opIdMem, "--nohttpinterface" );
    r = startMongod( "--port", rPort, "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + lpPort, "127.0.0.1:" + aPort, "--oplogSize", "1", "--opIdMem", opIdMem, "--nohttpinterface" );
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == -1 || lm == 0, "lm value invalid" );
                assert( rm == -1 || rm == 0 || rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    // now each can only talk to arbiter
    disconnect();    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 1 && rm == 1 );
                } );
    
    for( i = 0; i < nSlave; ++i ) {
        write( l, i, i );
    }    
    l.getDB( baseName ).getCollection( baseName ).findOne();

    for( i = 10000; i < 15000; ++i ) {
        write( r, i, i );
    }    
    r.getDB( baseName ).getCollection( baseName ).findOne();

    connect();
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );       

    r.getDB( baseName ).getCollection( baseName ).update( {_id:nSlave - 1}, {_id:nSlave - 1,n:-1}, true );
    assert.eq( -1, r.getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n );
    checkCount( r, 5000 + nSlave );
    assert.eq( -1, r.getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n );
    l.setSlaveOk();
    assert.soon( function() {
                n = l.getDB( baseName ).getCollection( baseName ).findOne( {_id:nSlave - 1} ).n;
                print( n );
                return -1 == n;
                } );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

doTest( 5000, 100000000 );
doTest( 5000, 100 ); // force op id converstion to collection based storage
