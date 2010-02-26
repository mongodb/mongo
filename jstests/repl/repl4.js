// Test replication 'only' mode

soonCount = function( db, coll, count ) {
    assert.soon( function() { 
                return s.getDB( db )[ coll ].find().count() == count; 
                } );    
}

doTest = function() {

    rt = new ReplTest( "repl4tests" );
    
    m = rt.start( true );
    s = rt.start( false, { only: "c" } );
    
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
    
    rt.stop( false );
    
    cm.save( { x:3 } );
    bm.save( { x:4 } );
    
    s = rt.start( false, { only: "c" }, true );
    soonCount( "c", "c", 2 );
}

doTest();
