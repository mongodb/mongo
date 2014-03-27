t = db.jstests_remove9;
t.drop();

js = "while( 1 ) { for( i = 0; i < 10000; ++i ) { db.jstests_remove9.save( {i:i} ); } db.jstests_remove9.remove( {i: {$gte:0} } ); }";
pid = startMongoProgramNoConnect( "mongo" , "--eval" , js , db ? db.getMongo().host : null );

for( var i = 0; i < 10000; ++i ) {
    t.remove( {i:Random.randInt( 10000 )} );
    assert.automsg( "!db.getLastError()" );
}

stopMongoProgramByPid( pid );