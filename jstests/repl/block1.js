
var rt = new ReplTest( "block1" );

m = rt.start( true );
s = rt.start( false );

dbm = m.getDB( "foo" );
dbs = s.getDB( "foo" );

tm = dbm.bar;
ts = dbs.bar;

for ( var i=0; i<1000; i++ ){
    tm.insert( { _id : i } );
    dbm.runCommand( { getlasterror : 1 , w : 2 } )
    assert.eq( i + 1 , ts.count() , "A" + i );
    assert.eq( i + 1 , tm.count() , "B" + i );
}

rt.stop();




