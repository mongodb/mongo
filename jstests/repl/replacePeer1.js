// test replace peer on master

var baseName = "jstests_replacepeer1test";

ismaster = function( n ) {
    im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
//    print( "ismaster: " + tojson( im ) );
    assert( im );
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
    s.setSlaveOk();
    assert.soon( function() {
                return 1 == getCount( s );
                } );
}

doTest = function( signal ) {

    ports = allocatePorts( 4 );

    a = new MongodRunner( ports[ 0 ], "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( ports[ 1 ], "/data/db/" + baseName + "-left", "127.0.0.1:" + ports[ 3 ], "127.0.0.1:" + ports[ 0 ] );
    r = new MongodRunner( ports[ 3 ], "/data/db/" + baseName + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] );

    rp = new ReplPair( l, r, a );
    rp.start();
    rp.waitForSteadyState( [ 1, 0 ], rp.right().host );
    
    checkWrite( rp.master(), rp.slave() );

    rp.killNode( rp.slave(), signal );

    writeOne( rp.master() );

    assert.commandWorked( rp.master().getDB( "admin" ).runCommand( {replacepeer:1} ) );

    rp.killNode( rp.master(), signal );
    rp.killNode( rp.arbiter(), signal );
    
    o = new MongodRunner( ports[ 2 ], "/data/db/" + baseName + "-left", "127.0.0.1:" + ports[ 3 ], "127.0.0.1:" + ports[ 0 ] );
    r = new MongodRunner( ports[ 3 ], "/data/db/" + baseName + "-right", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );

    rp = new ReplPair( o, r, a );
    resetDbpath( "/data/db/" + baseName + "-left" );
    rp.start( true );
    rp.waitForSteadyState( [ 1, 0 ], rp.right().host );

    checkWrite( rp.master(), rp.slave() );
    rp.slave().setSlaveOk();
    assert.eq( 3, rp.slave().getDB( baseName ).z.find().toArray().length );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
