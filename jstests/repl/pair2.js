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
    
    // spec small oplog for fast startup on 64bit machines
    a = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-arbiter" );
    l = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:27020", "127.0.0.1:27018", "--oplogSize", "1" );
    r = startMongod( "--port", "27020", "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:27019", "127.0.0.1:27018", "--oplogSize", "1" );
    l.setSlaveOk();
    
    assert.soon( function() { return ( ismaster( l ) == 0 && ismaster( r ) == 1 ); } );
    
    rz = r.getDB( baseName ).z
    
    rz.save( { _id: new ObjectId() } );
    soonCount( 1 );
    assert.eq( 0, l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    stopMongod( 27019, signal );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        rz.save( { _id: new ObjectId(), i: i, b: big } );
    
    l = startMongoProgram( "mongod", "--port", "27019", "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:27020", "127.0.0.1:27018", "--oplogSize", "1" );
    l.setSlaveOk();
    assert.soon( function() { return 1 == l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok; } );
    
    sleep( 3000 );
    
    soonCount( 1001 );
    lz = l.getDB( baseName ).z
    assert.eq( 1, lz.find( { i: 0 } ).count() );
    assert.eq( 1, lz.find( { i: 999 } ).count() );
    
    assert.eq( 0, l.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    stopMongod( 27018 );
    stopMongod( 27019 );
    stopMongod( 27020 );
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
