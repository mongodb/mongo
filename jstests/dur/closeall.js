// testing closealldatabases concurrency
// this is also a test of recoverFromYield() as that will get exercised by the update

function f() {
    var path = "/data/db/closeall";
    var path2 = "/data/db/closeall_slave";
    var ourdb = "closealltest";

    print("closeall.js start mongod");
    var options = (new Date()-0)%2==0 ? 8 : 0;
    print("closeall.js --durOptions " + options);
    var N = 1000;
    if (options) 
        N = 300;

    // use replication to exercise that code too with a close, and also to test local.sources with a close
    var conn = startMongodEmpty("--port", 30001, "--dbpath", path, "--dur", "--durOptions", options, "--master", "--oplogSize", 64);
    var connSlave = startMongodEmpty("--port", 30002, "--dbpath", path2, "--dur", "--durOptions", options, "--slave", "--source", "localhost:30001");

    var slave = connSlave.getDB(ourdb);

    // we'll use two connections to make a little parallelism
    var db1 = conn.getDB(ourdb);
    var db2 = new Mongo(db1.getMongo().host).getDB(ourdb);

    print("closeall.js run test");

    for( var i = 0; i < N; i++ ) { 
    	db1.foo.insert({x:1}); // this does wait for a return code so we will get some parallelism
	    if( i % 7 == 0 )
	        db1.foo.insert({x:99, y:2});
	    if( i %     49 == 0 )
	        db1.foo.update({ x: 99 }, { a: 1, b: 2, c: 3, d: 4 });
	    if (i % 100 == 0)
	        db1.foo.find();
	    if( i == 800 )
	        db1.foo.ensureIndex({ x: 1 });
        var res = null;
        try {
            res = db2.adminCommand("closeAllDatabases");
        }
        catch (e) {
            print("\n\n\nFAIL closeall.js closeAllDatabases command invocation threw an exception. i:" + i + "\n\n\n");
            try {
                print("getlasterror:");
                printjson(db2.getLastErrorObj());
                print("trying one more closealldatabases:");
                res = db2.adminCommand("closeAllDatabases");
                printjson(res);
            }
            catch (e) {
                print("got another exception : " + e);
            }
            throw e;
        }
	    assert( res.ok, "closeAllDatabases res.ok=false");
	}

	print("closeall.js end test loop.  slave.foo.count:");
	print(slave.foo.count());

	print("closeall.js shutting down servers");
	stopMongod(30002);
	stopMongod(30001);
}

f();
print("SUCCESS closeall.js");
