// Test replication 'only' mode

var baseName = "jstests_repl4test";

soonCount = function( db, coll, count ) {
    assert.soon( function() { 
                if ( -1 == s.getDBNames().indexOf( db ) )
                    return false;
                if ( -1 == s.getDB( db ).getCollectionNames().indexOf( coll ) )
                    return false;
                return s.getDB( db )[ coll ].find().count() == count; 
                } );    
}

doTest = function() {
    
    // spec small oplog for fast startup on 64bit machines
    m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-master", "--master", "--oplogSize", "1" );
    s = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-slave", "--slave", "--source", "127.0.0.1:27018", "--only", "c" );
    
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
}

doTest();
