// Test that when user flags are set on a collection,
// then collection is sharded, flags get carried over.
(function() {

    if (jsTest.options().storageEngine === "mmapv1") {
        // the dbname and collection we'll be working with
        var dbname = "testDB";
        var coll = "userFlagsColl";
        var ns = dbname + "." + coll;

        // First create fresh collection on a new standalone mongod that will become a shard.
        var newShardConn = MongoRunner.runMongod({"shardsvr": ""});
        var db1 = newShardConn.getDB(dbname);
        var t = db1.getCollection(coll);
        print(t);
        db1.getCollection(coll).drop();  // in case collection already existed
        db1.createCollection(coll);

        // Then verify the new collection has userFlags set to 0
        var collstats = db1.getCollection(coll).stats();
        print("*************** Fresh Collection Stats ************");
        printjson(collstats);
        assert.eq(collstats.userFlags, 1, "fresh collection doesn't have userFlags = 1 ");

        // Now we modify the collection with the usePowerOf2Sizes flag
        var res = db1.runCommand({"collMod": coll, "usePowerOf2Sizes": false});
        assert.eq(res.ok, 1, "collMod failed");

        // and insert some stuff, for the hell of it
        var numdocs = 20;
        for (i = 0; i < numdocs; i++) {
            assert.writeOK(db1.getCollection(coll).insert({_id: i}));
        }

        // Next verify that userFlags has changed to 0
        collstats = db1.getCollection(coll).stats();
        print("*************** Collection Stats After CollMod ************");
        printjson(collstats);
        assert.eq(collstats.userFlags, 0, "modified collection should have userFlags = 0 ");

        // start up a new sharded cluster, and add previous mongod
        var s = new ShardingTest({name: "user_flags", shards: 1});
        assert(s.admin.runCommand({addshard: newShardConn.host, name: "myShard"}).ok,
               "did not accept new shard");

        // enable sharding of the collection. Only 1 chunk initially, so move it to
        // other shard to create the collection on that shard
        s.adminCommand({enablesharding: dbname});
        s.adminCommand({shardcollection: ns, key: {_id: 1}});
        s.adminCommand({moveChunk: ns, find: {_id: 1}, to: "shard0000", _waitForDelete: true});

        print("*************** Collection Stats On Other Shard ************");
        var shard2 = s._connections[0].getDB(dbname);
        shard2stats = shard2.getCollection(coll).stats();
        printjson(shard2stats);

        assert.eq(shard2stats.count, numdocs, "moveChunk didn't succeed");
        assert.eq(shard2stats.userFlags, 0, "new shard should also have userFlags = 0 ");

        MongoRunner.stopMongod(newShardConn);
        s.stop();
    }

})();
