// pairing with auth

var baseName = "jstests_pair7test";

setAdmin = function( n ) {
    n.getDB( "admin" ).addUser( "super", "super" );
    n.getDB( "local" ).addUser( "repl", "foo" );
    n.getDB( "local" ).system.users.findOne();
}

auth = function( n ) {
    return n.getDB( baseName ).auth( "test", "test" );
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    m = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    setAdmin( m );
    stopMongod( ports[ 1 ] );

    m = startMongod( "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-right", "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    setAdmin( m );
    stopMongod( ports[ 2 ] );
    
    a = new MongodRunner( ports[ 0 ], "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( ports[ 1 ], "/data/db/" + baseName + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ], [ "--auth" ] );
    r = new MongodRunner( ports[ 2 ], "/data/db/" + baseName + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ], [ "--auth" ] ); 
    
    rp = new ReplPair( l, r, a );
    rp.start( true );
    rp.waitForSteadyState();

    rp.master().getDB( "admin" ).auth( "super", "super" );
    rp.master().getDB( baseName ).addUser( "test", "test" );
    auth( rp.master() ); // reauth
    assert.soon( function() { return auth( rp.slave() ); } );
    rp.slave().setSlaveOk();
    
    ma = rp.master().getDB( baseName ).a;
    ma.save( {} );
    sa = rp.slave().getDB( baseName ).a;
    assert.soon( function() { return 1 == sa.count(); } );
    
    rp.killNode( rp.slave(), signal );
    ma.save( {} );

    rp.start( true );
    rp.waitForSteadyState();
    assert.soon( function() { return auth( rp.slave() ); } );
    rp.slave().setSlaveOk();
    sa = rp.slave().getDB( baseName ).a;    
    assert.soon( function() { return 2 == sa.count(); } );
    
    ma.save( {a:1} );
    assert.soon( function() { return 1 == sa.count( {a:1} ); } );
    
    ma.update( {a:1}, {b:2} );
    assert.soon( function() { return 1 == sa.count( {b:2} ); } );
    
    ma.remove( {b:2} );
    assert.soon( function() { return 0 == sa.count( {b:2} ); } );
    
    rp.killNode( rp.master(), signal );
    rp.waitForSteadyState( [ 1, null ] );
    ma = sa;
    ma.save( {} );
    
    rp.start( true );
    rp.waitForSteadyState();
    assert.soon( function() { return auth( rp.slave() ); } );
    rp.slave().setSlaveOk();
    sa = rp.slave().getDB( baseName ).a;    
    assert.soon( function() { return 3 == sa.count(); } );
    
    ma.save( {} );
    assert.soon( function() { return 4 == sa.count(); } );    
    
    ports.forEach( function( x ) { stopMongod( x ); } );
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
