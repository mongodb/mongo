// data consistency after master-master

var baseName = "jstests_pair4test";

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

check = function( m, n, id ) {
    m.setSlaveOk();
    if ( id ) {
        find = { _id:id, n:n };
    } else {
        find = { n:n };
    }
    assert.soon( function() { return m.getDB( baseName ).getCollection( baseName ).find( find ).count() > 0; },
                "failed waiting for " + m + " value of n to be " + n );
}

checkCount = function( m, c ) {
    m.setSlaveOk();
    assert.soon( function() {
                actual = m.getDB( baseName ).getCollection( baseName ).find().count();
                print( actual );
                return c == actual; },
                "count failed for " + m );
}

coll = function( m ) {
    return m.getDB( baseName ).getCollection( baseName );
}

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
    
    coll( r ).ensureIndex( {_id:1} );
    
    write( r, 0 );
    write( r, 1 );
    check( l, 0 );
    check( l, 1 );
    
    // now each can only talk to arbiter
    disconnect();    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 1 && rm == 1 );
                } );
    
    write( r, 10 );
    check( r, 10 );
    write( r, 100, "a" );
    check( r, 100, "a" );
    coll( r ).update( {n:1}, {$set:{n:2}} );
    
    write( l, 20 );
    check( l, 20 );
    write( l, 200, "a" );
    check( l, 200, "a" );
    coll( l ).update( {n:1}, {$set:{m:3}} );
        
    // recover
    connect();
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    checkCount( r, 5 );
    checkCount( l, 5 );
    [ r, l ].forEach( function( x ) { [ 0, 10, 20, 100 ].forEach( function( y ) { check( x, y ); } ); } );
    
    checkM = function( c ) {
        assert.soon( function() {
                    obj = coll( c ).findOne( {n:2} );
                    printjson( obj );
                    return obj.m == undefined;
                    }, "n:2 test for " + c + " failed" );
    }
    checkM( r );
    checkM( l );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

doTest();