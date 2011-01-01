baseName = "jstests_shellkillop";

db[ baseName ].drop();

print("shellkillop.js insert data");
for (i = 0; i < 100000; ++i) {
    db[ baseName ].save( {i:1} );
}
assert.eq( 100000, db[ baseName ].count() );

var evalStr = "print('SKO subtask started'); db." + baseName + ".update( {}, {$set:{i:'abcdefghijkl'}}, false, true ); db." + baseName + ".count();";
print("shellkillop.js evalStr:" + evalStr);
spawn = startMongoProgramNoConnect("mongo", "--autokillop", "--port", myPort(), "--eval", evalStr);

sleep(100);

stopMongoProgramByPid(spawn);

sleep(200);
var inprog = db.currentOp().inprog;
if (inprog.count) {
    sleep(2000); // wait more if need be
    inprog = db.currentOp().inprog;
}
for( i in inprog ) {
    if (inprog[i].ns == "test." + baseName) {
        print("shellkillop.js FAIL op is still running:");
        printjson(inprog);
        assert(false, "still running op");
    }
}

print("shellkillop.js SUCCESS");
