// Pairing resync

var baseName = "jstests_pair2test";

ismaster = function( n ) {
    im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
    assert( im );
    return im.ismaster;
}

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "counting" );
                if ( -1 == l.getDBNames().indexOf( baseName ) )
                    return false;
                if ( -1 == l.getDB( baseName ).getCollectionNames().indexOf( "z" ) )
                    return false;
//                print( "counted: " + l.getDB( baseName ).z.find().count() );
                return l.getDB( baseName ).z.find().count() == count; 
                } );    
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    // spec small oplog for fast startup on 64bit machines
    a = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    r = startMongod( "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    l.setSlaveOk();
    
    assert.soon( function() { return ( ismaster( l ) == 0 && ismaster( r ) == 1 ); } );
    
    rz = r.getDB( baseName ).z
    
    rz.save( { _id: new ObjectId() } );
    sleep( 5000 );
    soonCount( 1 );
    assert.eq( 0, l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    stopMongod( ports[ 1 ], signal );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        rz.save( { _id: new ObjectId(), i: i, b: big } );
    
    l = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    l.setSlaveOk();
    assert.soon( function() { return 1 == l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok; } );
    
    sleep( 8000 );
    soonCount( 1001 );
    lz = l.getDB( baseName ).z
    assert.eq( 1, lz.find( { i: 0 } ).count() );
    assert.eq( 1, lz.find( { i: 999 } ).count() );
    
    assert.eq( 0, l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
