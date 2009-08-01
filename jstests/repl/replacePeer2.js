// test replace peer

var baseName = "jstests_replacepeer2test";

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

    // spec small oplog for fast startup on 64bit machines
    a = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    l = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 3 ], "--arbiter", "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    r = startMongod( "--port", ports[ 3 ], "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + ports[ 1 ], "--arbiter", "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );

    assert.soon( function() {
                am = ismaster( a );
                lm = ismaster( l );
                rm = ismaster( r );

                assert( am == 1 );
                assert( lm == -1 || lm == 0 );
                assert( rm == -1 || rm == 0 || rm == 1 );

                return ( lm == 0 && rm == 1 );
                } );

    checkWrite( r, l );

    // allow slave to finish initial sync
    assert.soon( function() { return 1 == l.getDB( "admin" ).runCommand( {replacepeer:1} ).ok; } );

    // Should not be saved to l.
    writeOne( r );
    // Make sure there would be enough time to save to l if we hadn't called replacepeer.
    sleep( 10000 );

    stopMongod( ports[ 3 ], signal );
    stopMongod( ports[ 1 ], signal );

    l = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 2 ], "--arbiter", "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    r = startMongod( "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + ports[ 1 ], "--arbiter", "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );

    assert.soon( function() {
                am = ismaster( a );
                lm = ismaster( l );
                rm = ismaster( r );

                assert( am == 1 );
                assert( lm == -1 || lm == 0 || lm == 1 );
                assert( rm == -1 || rm == 0 );

                return ( lm == 1 && rm == 0 );
                } );

    checkWrite( l, r );
    r.setSlaveOk();
    assert.eq( 2, r.getDB( baseName ).z.find().toArray().length );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
