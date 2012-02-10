// testing closealldatabases concurrency
// this is also a test of recoverFromYield() as that will get exercised by the update

function f() {
    var variant = (new Date()) % 4;
    var path = "/data/db/closeall";
    var path2 = "/data/db/closeall_slave";
    var ourdb = "closealltest";

    print("closeall.js start mongod variant:" + variant);
    var R = (new Date()-0)%2;
    var QuickCommits = (new Date()-0)%3 == 0;
    var options = R==0 ? 8 : 0; // 8 is DurParanoid
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
    if( QuickCommits ) {
        print("closeall.js QuickCommits variant (using a small syncdelay)");
        assert( db2.adminCommand({setParameter:1, syncdelay:5}).ok );
    }

    print("closeall.js run test");

    print("wait for initial sync to finish") // SERVER-4852
    db1.foo.insert({});
    err = db1.getLastErrorObj(2);
    printjson(err)
    assert.isnull(err.err);
    db1.foo.remove({});
    err = db1.getLastErrorObj(2);
    printjson(err)
    assert.isnull(err.err);
    print("initial sync done")

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
            if( variant == 1 )
                sleep(0);
            else if( variant == 2 ) 
                sleep(1);
            else if( variant == 3 && i % 10 == 0 )
                print(i);
            res = db2.adminCommand("closeAllDatabases");
        }
        catch (e) {
            sleep(5000); // sleeping a little makes console output order prettier
            print("\n\n\nFAIL closeall.js closeAllDatabases command invocation threw an exception. i:" + i);
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
            print("\n\n\n");
            // sleep a little to capture possible mongod output?
            sleep(2000);
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
sleep(500);
print("SUCCESS closeall.js");
