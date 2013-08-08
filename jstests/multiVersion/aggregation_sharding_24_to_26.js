// This tests running aggregations during an upgrade from 2.4 to 2.6
function testVersions(versions) {
    function runTest(coll, testFailures) {
        var bigStr = Array(1000).toString();
        for (var i=0; i< 1000; i++) {
            coll.insert({_id: i, bigStr:bigStr});
        }

        // wait for all writebacks to be applied
        assert.eq(coll.getDB().getLastError(), null);


        // basic aggregation using just 2.4 features works
        var res = coll.runCommand('aggregate', {pipeline: [{$group: {_id: null,
                                                                     count: {$sum: 1},
                                                                     avg: {$avg: '$_id'},
                                                                     }}]});
        assert.eq(res.result, [{_id: null, count: 1000, avg: 499.5}]);

        // Targeted aggregation works
        var res = coll.runCommand('aggregate', {pipeline: [{$match: {_id: 0}},
                                                           {$project: {_id: 1}},
                                                          ]});
        assert.eq(res.result, [{_id: 0}]);

        if (testFailures) {
            // 2.6 features aren't guaranteed to work until upgrade is complete. They may work
            // anyway if all data is on upgraded shards and the primary is updated, which is why
            // these only run in the "normal" sharded case
            assert.throws(function() {coll.aggregateCursor({$limit: 100000})});
            assert.commandFailed(coll.runCommand('aggregate', {pipeline: [{$out: "ts1_out"}]}));
            assert.commandFailed(coll.runCommand('aggregate', {pipeline: [{$limit: 10}],
                                                            allowDiskUsage:true}));
        }
    }

    jsTest.log("Starting test with versions: " + tojson(versions));

    var st = new ShardingTest({
        shards: 2,
        // verbose: 2, // enable for debugging
        mongos: 1,
        other: {
            chunksize: 1,
            separateConfig: true,
            mongosOptions: {binVersion: versions.mongos},
            shardOptions: {binVersion: MongoRunner.versionIterator(versions.shards)},
        },
    });

    var mongos = st.s0;
    var coll = mongos.getCollection("test.collection");
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();

    // show all exceptions in mongod
    for (var i = 0; i < shards.length; i++) {
        var shardConn = new Mongo(shards[i].host);
        assert.commandWorked(shardConn.adminCommand({setParameter: 1, traceExceptions: true}));
    }

    jsTest.log("About to test unsharded db. Versions: " +  tojson(versions));
    coll.getDB().dropDatabase();
    runTest(coll, false);

    jsTest.log("About to test sharded db unsharded collection. Versions: " +  tojson(versions));
    coll.getDB().dropDatabase();
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
    runTest(coll, false);

    jsTest.log("About to test normal sharded collection with balancer on. " +
               "Versions: " +  tojson(versions));
    coll.getDB().dropDatabase();
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
    assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    runTest(coll, true);

    // Later tests don't want the balancer on
    st.stopBalancer();

    jsTest.log("About to test sharded collection with all data on primary shard. " +
               "Versions: " +  tojson(versions));
    coll.getDB().dropDatabase();
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
    // movePrimary fails if the "to" shard is already primary. That is successful for our purposes.
    printjson(admin.runCommand({movePrimary: coll.getDB().getName(), to: shards[0]._id}));
    assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    runTest(coll, false);

    jsTest.log("About to test sharded collection with all data NOT on primary shard. " +
               "Versions: " +  tojson(versions));
    coll.getDB().dropDatabase();
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
    printjson(admin.runCommand({movePrimary: coll.getDB().getName(), to: shards[0]._id}));
    assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({moveChunk: coll.getFullName(),
                                           find: {_id: 1}, // should find the only chunk
                                           to: shards[1]._id}));
    runTest(coll, false);

    // terminate all servers
    st.stop();
    jsTest.log("Passed test with versions: " +  tojson(versions));
}

testVersions({mongos: '2.6', shards: '2.4'});
testVersions({mongos: '2.6', shards: ['2.6', '2.4']});
testVersions({mongos: '2.6', shards: ['2.4', '2.6']});

// Not an officially supported configuration, but should work anyway.
testVersions({mongos: '2.4', shards: '2.6'});
