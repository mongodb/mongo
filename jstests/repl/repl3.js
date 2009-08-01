// Test autoresync

var baseName = "jstests_repl3test";

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "check count" );
//                print( "count: " + s.getDB( baseName ).z.find().count() );
                return s.getDB( baseName ).a.find().count() == count; 
                } );    
}

doTest = function( signal ) {

    ports = allocatePorts( 2 );
    
    // spec small oplog to make slave get out of sync
    m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    am = m.getDB( baseName ).a
    
    am.save( { _id: new ObjectId() } );
    soonCount( 1 );
    stopMongod( ports[ 1 ], signal );
    
    big = new Array( 2000 ).toString();
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i, b: big } );
    
    s = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--autoresync", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    // after SyncException, mongod waits 10 secs.
    sleep( 15000 );
    
    // Need the 2 additional seconds timeout, since commands don't work on an 'allDead' node.
    soonCount( 1001 );
    as = s.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );
    
    assert.eq( 0, s.getDB( "admin" ).runCommand( { "resync" : 1 } ).ok );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
