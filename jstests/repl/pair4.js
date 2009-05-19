// data consistency after master-master

var baseName = "jstests_pair4test";

debug = function( o ) {
    printjson( o );
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

db2Coll = function( m ) {
    return m.getDB( baseName + "_second" ).getCollection( baseName );    
}

doTest = function( recover, newMaster, newSlave ) {
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
    
    firstMaster = pair.master();
    firstSlave = pair.slave();
        
    write( pair.master(), 0 );
    write( pair.master(), 1 );
    check( pair.slave(), 0 );
    check( pair.slave(), 1 );
    
    // now each can only talk to arbiter
    disconnect();    
    pair.waitForSteadyState( [ 1, 1 ], null, true );
    
    m = newMaster();
    write( m, 10 );
    write( m, 100, "a" );
    coll( m ).update( {n:1}, {$set:{n:2}} );
    db2Coll( m ).save( {n:500} );
    db2Coll( m ).findOne();
    
    s = newSlave();
    write( s, 20 );
    write( s, 200, "a" );
    coll( s ).update( {n:1}, {n:1,m:3} );
    db2Coll( s ).save( {_id:"a",n:600} );
    db2Coll( s ).findOne();
        
    // recover
    recover();
    
    nodes = [ pair.right(), pair.left() ];
    
    nodes.forEach( function( x ) { checkCount( x, 5 ); } );
    nodes.forEach( function( x ) { [ 0, 10, 20, 100 ].forEach( function( y ) { check( x, y ); } ); } );
    
    checkM = function( c ) {
        assert.soon( function() {
                    obj = coll( c ).findOne( {n:2} );
                    printjson( obj );
                    return obj.m == undefined;
                    }, "n:2 test for " + c + " failed" );
    };
    nodes.forEach( function( x ) { checkM( x ); } );

    // check separate database
    nodes.forEach( function( x ) { assert.soon( function() {
                                                  r = db2Coll( x ).findOne( {_id:"a"} );
                                                  debug( r );
                                                  if ( r == null ) {
                                                      return false;
                                                  }
                                                  return 600 == r.n;
                                                  } ) } );
    
    ports.forEach( function( x ) { stopMongoProgram( x ); } );
    
}

debug( "basic test" );
doTest( function() {
       connect();
       pair.waitForSteadyState( [ 1, 0 ], pair.right().host, true );
       }, function() { return pair.right(); }, function() { return pair.left(); } );

doRestartTest = function( signal ) {    
    doTest( function() {
           if ( signal == 9 ) {
                sleep( 3000 );
           }
           pair.killNode( firstMaster, signal );
           connect();
           pair.start( true );
           pair.waitForSteadyState( [ 1, 0 ], firstSlave.host, true );
           }, function() { return firstSlave; }, function() { return firstMaster; } );
}

debug( "sigterm restart test" );
doRestartTest( 15 ) // SIGTERM

debug( "sigkill restart test" );
doRestartTest( 9 ) // SIGKILL
