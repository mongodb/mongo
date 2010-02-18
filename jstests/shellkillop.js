baseName = "jstests_shellkillop";

db[ baseName ].drop();

for( i = 0; i < 100000; ++i ) {
    db[ baseName ].save( {i:1} );
}
assert.eq( 100000, db[ baseName ].count() );

spawn = startMongoProgramNoConnect( "mongo", "--port", myPort(), "--eval", "db." + baseName + ".update( {}, {$set:{i:\"abcdefghijkl\"}}, false, true ); db." + baseName + ".count();" );
sleep( 100 );
stopMongoProgramByPid( spawn );
sleep( 100 );
assert.eq( [], db.currentOp().inprog );