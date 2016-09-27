// Test disabled until SERVER-3853 is finished
if (0) {
    (function() {

        var name = "dumprestore9";
        function step(msg) {
            msg = msg || "";
            this.x = (this.x || 0) + 1;
            print('\n' + name + ".js step " + this.x + ' ' + msg);
        }

        var s = new ShardingTest({
            name: "dumprestore9a",
            shards: 2,
            mongos: 3,
            other: {chunkSize: 1, enableBalancer: true}
        });

        step("Shard collection");

        s.adminCommand({
            enablesharding: "aaa"
        });  // Make this db alphabetically before 'config' so it gets restored first
        s.ensurePrimaryShard('aaa', 'shard0001');
        s.adminCommand({shardcollection: "aaa.foo", key: {x: 1}});

        db = s.getDB("aaa");
        coll = db.foo;

        step("insert data");

        str = 'a';
        while (str.length < 1024 * 512) {
            str += str;
        }

        numDocs = 20;
        for (var i = 0; i < numDocs; i++) {
            coll.insert({x: i, str: str});
        }

        step("Wait for balancing");

        assert.soon(function() {
            var x = s.chunkDiff("foo", "aaa");
            print("chunk diff: " + x);
            return x < 2;
        }, "no balance happened", 8 * 60 * 1000, 2000);

        assert.eq(numDocs, coll.count(), "Documents weren't inserted correctly");

        step("dump cluster");

        dumpdir = MongoRunner.dataDir + "/dumprestore9-dump1/";
        resetDbpath(dumpdir);
        var exitCode = MongoRunner.runMongoTool("mongodump", {
            host: s.s0.host,
            out: dumpdir,
        });
        assert.eq(0, exitCode, "mongodump failed to dump data through one of the mongos processes");

        step("Shutting down cluster");

        s.stop();

        step("Starting up clean cluster");
        s = new ShardingTest({name: "dumprestore9b", shards: 2, mongos: 3, other: {chunkSize: 1}});

        db = s.getDB("aaa");
        coll = db.foo;

        assert.eq(0, coll.count(), "Data wasn't cleaned up by restarting sharding test");

        step("Restore data and config");

        exitCode = MongoRunner.runMongoTool("mongorestore", {
            dir: dumpdir,
            host: s.s1.host,
            restoreShardingConfig: "",
            forceConfigRestore: "",
        });
        assert.eq(
            0, exitCode, "mongorestore failed to restore data through the other mongos process");

        config = s.getDB("config");
        assert(config.databases.findOne({_id: 'aaa'}).partitioned,
               "Config data wasn't restored properly");

        assert(s.chunkDiff("foo", "aaa") < 2, "Chunk data wasn't restored properly");

        assert.eq(numDocs, coll.count(), "Didn't restore all documents properly2");
        assert.eq(numDocs, coll.find().itcount(), "Didn't restore all documents properly");

        for (var i = 0; i < numDocs; i++) {
            doc = coll.findOne({x: i});
            assert.eq(i, doc.x, "Doc missing from the shard it should be on");
        }

        for (var i = 0; i < s._connections.length; i++) {
            assert(s._connections[i].getDB("aaa").foo.count() > 0,
                   "No data on shard: " + s._connections[i].host);
        }

        step("Stop cluster");
        s.stop();
        step("SUCCESS");

    })();
}
