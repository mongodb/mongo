// testing closealldatabases concurrency
// this is also a test of recoverFromYield() as that will get exercised by the update

function f() {
    var path = "/data/db/closeall";
    var ourdb = "closealltest";

    print("closeall.js start mongod");
    var options = Math.random() < 0.5 ? 24 : 0;
    print("closeall.js --durOptions " + options);

    var conn = startMongodEmpty("--port", 30001, "--dbpath", path, "--dur", "--durOptions", options);

    // we'll use two connections to make a little parallelism
    var db1 = conn.getDB(ourdb);
    var db2 = new Mongo(db1.getMongo().host).getDB(ourdb);

    print("closeall.js run test");

    for( var i = 0; i < 1000; i++ ) { 
    	db1.foo.insert({x:1}); // this does wait for a return code so we will get some parallelism
	    if( i % 7 == 0 )
	        db1.foo.insert({x:99, y:2});
	    if( i %     49 == 0 )
	        db1.foo.update({x:99},{a:1,b:2,c:3,d:4});
	    if( i == 800 )
	        db1.foo.ensureIndex({x:1});
	    var res = db2.adminCommand("closeAllDatabases");
	    assert( res.ok, "closeAllDatabases res.ok=false");
    }
}

f();
print("SUCCESS closeall.js");
