// Pairing resync

var baseName = "jstests_pair2test";

ismaster = function( n ) {
    im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
    assert( im );
    return im.ismaster;
}

soonCount = function( m, count ) {
    assert.soon( function() { 
//                print( "counting" );
////                print( "counted: " + l.getDB( baseName ).z.find().count() );
                return m.getDB( baseName ).z.find().count() == count; 
                } );    
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    a = new MongodRunner( ports[ 0 ], "/data/db/" + baseName + "-arbiter" );
    l = new MongodRunner( ports[ 1 ], "/data/db/" + baseName + "-left", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ] );
    r = new MongodRunner( ports[ 2 ], "/data/db/" + baseName + "-right", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ] ); 
    
    rp = new ReplPair( l, r, a );
    rp.start();
    rp.waitForSteadyState();

    rp.slave().setSlaveOk();
    mz = rp.master().getDB( baseName ).z;
    
    mz.save( { _id: new ObjectId() } );
    soonCount( rp.slave(), 1 );
    assert.eq( 0, rp.slave().getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    sleep( 3000 ); // allow time to finish clone and save ReplSource
    rp.killNode( rp.slave(), signal );
    rp.waitForSteadyState( [ 1, null ], rp.master().host );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        mz.save( { _id: new ObjectId(), i: i, b: big } );

    rp.start( true );
    rp.waitForSteadyState( [ 1, 0 ], rp.master().host );
    
    sleep( 15000 );
    
    rp.slave().setSlaveOk();
    assert.soon( function() {
                ret = rp.slave().getDB( "admin" ).runCommand( { "resync" : 1 } );
//                printjson( ret );
                return 1 == ret.ok;
                } );
    
    sleep( 8000 );
    soonCount( rp.slave(), 1001 );
    sz = rp.slave().getDB( baseName ).z
    assert.eq( 1, sz.find( { i: 0 } ).count() );
    assert.eq( 1, sz.find( { i: 999 } ).count() );
    
    assert.eq( 0, rp.slave().getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
