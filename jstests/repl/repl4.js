// Test replication 'only' mode

var baseName = "jstests_repl4test";

soonCount = function( db, coll, count ) {
    assert.soon( function() { 
                return s.getDB( db )[ coll ].find().count() == count; 
                } );    
}

doTest = function() {

    ports = allocatePorts( 2 );
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    s = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--only", "c", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    
    cm = m.getDB( "c" ).c
    bm = m.getDB( "b" ).b
    
    cm.save( { x:1 } );
    bm.save( { x:2 } );

    soonCount( "c", "c", 1 );
    assert.eq( 1, s.getDB( "c" ).c.findOne().x );
    sleep( 10000 );
    printjson( s.getDBNames() );
    assert.eq( -1, s.getDBNames().indexOf( "b" ) );
    assert.eq( 0, s.getDB( "b" ).b.find().count() );
    
    stopMongod( ports[ 1 ] );
    
    cm.save( { x:3 } );
    bm.save( { x:4 } );
    
    s = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:" + ports[ 0 ], "--only", "c", "--nohttpinterface", "--noprealloc", "--bind_ip", "127.0.0.1" );
    soonCount( "c", "c", 2 );
}

doTest();
