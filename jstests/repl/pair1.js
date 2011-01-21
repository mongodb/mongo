// Basic pairing test

var baseName = "jstests_pair1test";

debug = function( p ) {
//    print( p );
}

ismaster = function( n ) {
    var im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
//    print( "ismaster: " + tojson( im ) );
    assert( im, "command ismaster failed" );
    return im.ismaster;
}

var writeOneIdx = 0;

writeOne = function( n ) {
    n.getDB( baseName ).z.save( { _id: new ObjectId(), i: ++writeOneIdx } );
}

getCount = function( n ) {
    return n.getDB( baseName ).z.find( { i: writeOneIdx } ).toArray().length;
}

checkWrite = function( m, s ) {
    writeOne( m );
    assert.eq( 1, getCount( m ) );
    check( s );
}

check = function( s ) {
    s.setSlaveOk();
    assert.soon( function() {
                return 1 == getCount( s );
                } );    
    sleep( 500 ); // wait for sync clone to finish up
}

// check that slave reads and writes are guarded
checkSlaveGuard = function( s ) {
    var t = s.getDB( baseName + "-temp" ).temp;
    assert.throws( t.find().count, [], "not master" );
    assert.throws( t.find(), [], "not master", "find did not assert" );
    
    checkError = function() {
        assert.eq( "not master", s.getDB( "admin" ).getLastError() );
        s.getDB( "admin" ).resetError();
    }
    s.getDB( "admin" ).resetError();
    t.save( {x:1} );
    checkError();
    t.update( {}, {x:2}, true );
    checkError();
    t.remove( {x:0} );
    checkError();
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    a = new MongodRunner( ports[ 0 ], "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( ports[ 1 ], "/data/db/" + baseName + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );
    r = new MongodRunner( ports[ 2 ], "/data/db/" + baseName + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] ); 

    rp = new ReplPair( l, r, a );
    rp.start();
    rp.waitForSteadyState();
    
    checkSlaveGuard( rp.slave() );
    
    checkWrite( rp.master(), rp.slave() );
    
    debug( "kill first" );
    rp.killNode( rp.master(), signal );
    rp.waitForSteadyState( [ 1, null ], rp.slave().host );
    writeOne( rp.master() );
    
    debug( "restart first" );
    rp.start( true );
    rp.waitForSteadyState();
    check( rp.slave() );
    checkWrite( rp.master(), rp.slave() );

    debug( "kill second" );
    rp.killNode( rp.master(), signal );
    rp.waitForSteadyState( [ 1, null ], rp.slave().host );

    debug( "restart second" );
    rp.start( true );
    rp.waitForSteadyState( [ 1, 0 ], rp.master().host );
    checkWrite( rp.master(), rp.slave() );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
