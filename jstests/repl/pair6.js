// pairing cases where oplogs run out of space

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

checkCount = function( m, c ) {
    m.setSlaveOk();
    assert.soon( function() {
                actual = m.getDB( baseName ).getCollection( baseName ).find().count();
                print( actual );
                return c == actual; },
                "expected count " + c + " for " + m );
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
    a = startMongod( "--port", aPort, "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", lPort, "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + rpPort, "127.0.0.1:" + aPort, "--oplogSize", "1", "--nohttpinterface" );
    r = startMongod( "--port", rPort, "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + lpPort, "127.0.0.1:" + aPort, "--oplogSize", "1", "--nohttpinterface" );
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == -1 || lm == 0, "lm value invalid" );
                assert( rm == -1 || rm == 0 || rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );

    disconnect();    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 1 && rm == 1 );
                } );

    print( "test one" );
    
    // fill slave oplog
    for( i = 0; i < 1000; ++i ) {
        l.getDB( baseName ).getCollection( baseName ).save( {b:big} );
    }
    l.getDB( baseName ).getCollection( baseName ).findOne();
    
    // write single to master
    r.getDB( baseName ).getCollection( baseName ).save( {} );
    
    connect();
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );       

    checkCount( l, 1 );
    checkCount( r, 1 );
    
    r.getDB( baseName ).getCollection( baseName ).remove( {} );
    checkCount( l, 0 );
    
    disconnect();    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 1 && rm == 1 );
                } );

    print( "test two" );
    
    // fill master oplog
    for( i = 0; i < 1000; ++i ) {
        r.getDB( baseName ).getCollection( baseName ).save( {b:big} );
    }

    l.getDB( baseName ).getCollection( baseName ).save( {_id:"abcde"} );

    connect();
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
              
                return ( lm == 0 && rm == 1 );
                } );       
    
    sleep( 30000 );
    
    checkCount( l, 1000 );
    checkCount( r, 1000 );
    assert.eq( 0, l.getDB( baseName ).getCollection( baseName ).find( {_id:"abcde"} ).count() );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

doTest();