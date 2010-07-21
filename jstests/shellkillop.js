baseName = "jstests_shellkillop";

db[ baseName ].drop();

for( i = 0; i < 100000; ++i ) {
    db[ baseName ].save( {i:1} );
}
assert.eq( 100000, db[ baseName ].count() );

spawn = startMongoProgramNoConnect( "mongo", "--autokillop", "--port", myPort(), "--eval", "db." + baseName + ".update( {}, {$set:{i:\"abcdefghijkl\"}}, false, true ); db." + baseName + ".count();" );
sleep( 100 );
stopMongoProgramByPid( spawn );
sleep( 100 );
inprog = db.currentOp().inprog
printjson( inprog );
for( i in inprog ) {
    assert( inprog[ i ].ns != "test." + baseName, "still running op" );
}
