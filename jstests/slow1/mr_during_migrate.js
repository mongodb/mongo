// Do parallel ops with migrates occurring
(function() {
    'use strict';

    var st = new ShardingTest({shards: 10, mongos: 2, verbose: 2});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var coll = st.s.getCollection(jsTest.name() + ".coll");

    var numDocs = 1024 * 1024;
    var dataSize = 1024;  // bytes, must be power of 2

    var data = "x";
    while (data.length < dataSize)
        data += data;

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i, data: data});
    }
    assert.writeOK(bulk.execute());

    // Make sure everything got inserted
    assert.eq(numDocs, coll.find().itcount());

    jsTest.log("Inserted " + sh._dataFormat(dataSize * numDocs) + " of data.");

    // Shard collection
    st.shardColl(coll, {_id: 1}, false);

    st.printShardingStatus();

    jsTest.log("Sharded collection now initialized, starting migrations...");

    var checkMigrate = function() {
        print("Result of migrate : ");
        printjson(this);
    };

    // Creates a number of migrations of random chunks to diff shard servers
    var ops = [];
    for (var i = 0; i < st._connections.length; i++) {
        ops.push({
            op: "command",
            ns: "admin",
            command: {
                moveChunk: "" + coll,
                find: {_id: {"#RAND_INT": [0, numDocs]}},
                to: st._connections[i].shardName,
                _waitForDelete: true
            },
            showResult: true
        });
    }

    // TODO: Also migrate output collection

    jsTest.log("Starting migrations now...");

    var bid = benchStart({ops: ops, host: st.s.host, parallel: 1, handleErrors: false});

    //#######################
    // Tests during migration

    var numTests = 5;

    for (var t = 0; t < numTests; t++) {
        jsTest.log("Test #" + t);

        var mongos = st.s1;  // use other mongos so we get stale shard versions
        var coll = mongos.getCollection(coll + "");
        var outputColl = mongos.getCollection(coll + "_output");

        var numTypes = 32;
        var map = function() {
            emit(this._id % 32 /* must be hardcoded */, {c: 1});
        };

        var reduce = function(k, vals) {
            var total = 0;
            for (var i = 0; i < vals.length; i++)
                total += vals[i].c;
            return {c: total};
        };

        printjson(coll.find({_id: 0}).itcount());

        jsTest.log("Starting new mapReduce run #" + t);

        // assert.eq( coll.find().itcount(), numDocs )

        coll.getMongo().getDB("admin").runCommand({setParameter: 1, traceExceptions: true});

        printjson(coll.mapReduce(
            map, reduce, {out: {replace: outputColl.getName(), db: outputColl.getDB() + ""}}));

        jsTest.log("MapReduce run #" + t + " finished.");

        assert.eq(outputColl.find().itcount(), numTypes);

        outputColl.find().forEach(function(x) {
            assert.eq(x.value.c, numDocs / numTypes);
        });
    }

    printjson(benchFinish(bid));

    st.stop();
})();
