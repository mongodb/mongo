// testing closealldatabases concurrency
// this is also a test of saveState() as that will get exercised by the update

function f(variant, quickCommits, paranoid) {
    var path = MongoRunner.dataDir + "/closeall";
    var path2 = MongoRunner.dataDir + "/closeall_slave";
    var ourdb = "closealltest";

    print("closeall.js start mongod variant:" + variant + "." + quickCommits + "." + paranoid);
    var options = (paranoid==1 ? 8 : 0); // 8 is DurParanoid
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
    if( quickCommits ) {
        print("closeall.js QuickCommits variant (using a small syncdelay)");
        assert( db2.adminCommand({setParameter:1, syncdelay:5}).ok );
    }

    print("closeall.js run test");

    print("wait for initial sync to finish") // SERVER-4852
    assert.writeOK(db1.foo.insert({}, { writeConcern: { w: 2 }}));
    assert.writeOK(db1.foo.remove({}, { writeConcern: { w: 2 }}));
    print("initial sync done")

    var writeOps = startParallelShell('var coll = db.getSiblingDB("closealltest").foo; \
                                       var bulk = coll.initializeUnorderedBulkOp(); \
                                       for( var i = 0; i < ' + N + '; i++ ) { \
                                           bulk.insert({ x: 1 }); \
                                           if ( i % 7 == 0 ) \
                                               bulk.insert({ x: 99, y: 2 }); \
                                           if ( i % 49 == 0 ) \
                                               bulk.find({ x: 99 }).update( \
                                                   { a: 1, b: 2, c: 3, d: 4 }); \
                                           if( i == 800 ) \
                                               coll.ensureIndex({ x: 1 }); \
                                       }', 30001);

    for( var i = 0; i < N; i++ ) {
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
            throw e;
        }
        assert( res.ok, "closeAllDatabases res.ok=false");
    }

    print("closeall.js end test loop.  slave.foo.count:");
    print(slave.foo.count());

    print("closeall.js shutting down servers");
    stopMongod(30002);
    stopMongod(30001);

    writeOps();
}

// Skip this test on 32-bit Windows (unfixable failures in MapViewOfFileEx)
//
if (_isWindows() && getBuildInfo().bits == 32 ) {
    print("Skipping closeall.js on 32-bit Windows");
}
else {
    for (var variant=0; variant < 4; variant++){
        for (var quickCommits=0; quickCommits <= 1; quickCommits++){ // false then true
            for (var paranoid=0; paranoid <= 1; paranoid++){ // false then true
                f(variant, quickCommits, paranoid);
                sleep(500);
            }
        }
    }
    print("SUCCESS closeall.js");
}
