(function() {

    var st =
        new ShardingTest({name: 'migrateBig_balancer', shards: 2, other: {enableBalancer: true}});
    var mongos = st.s;

    var admin = mongos.getDB("admin");
    db = mongos.getDB("test");
    var coll = db.getCollection("stuff");

    assert.commandWorked(admin.runCommand({enablesharding: coll.getDB().getName()}));
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');

    var data = "x";
    var nsq = 16;
    var n = 255;

    for (var i = 0; i < nsq; i++)
        data += data;

    dataObj = {};
    for (var i = 0; i < n; i++)
        dataObj["data-" + i] = data;

    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 40; i++) {
        bulk.insert({data: dataObj});
    }
    assert.writeOK(bulk.execute());

    assert.eq(40, coll.count(), "prep1");

    printjson(coll.stats());

    admin.printShardingStatus();

    admin.runCommand({shardcollection: "" + coll, key: {_id: 1}});

    assert.lt(
        5, mongos.getDB("config").chunks.find({ns: "test.stuff"}).count(), "not enough chunks");

    assert.soon(function() {
        // On *extremely* slow or variable systems, we've seen migrations fail in the critical
        // section and
        // kill the server.  Do an explicit check for this. SERVER-8781
        // TODO: Remove once we can better specify what systems to run what tests on.
        try {
            assert.commandWorked(st.shard0.getDB("admin").runCommand({ping: 1}));
            assert.commandWorked(st.shard1.getDB("admin").runCommand({ping: 1}));
        } catch (e) {
            print("An error occurred contacting a shard during balancing," +
                  " this may be due to slow disk I/O, aborting test.");
            throw e;
        }

        res = mongos.getDB("config").chunks.group({
            cond: {ns: "test.stuff"},
            key: {shard: 1},
            reduce: function(doc, out) {
                out.nChunks++;
            },
            initial: {nChunks: 0}
        });

        printjson(res);
        return res.length > 1 && Math.abs(res[0].nChunks - res[1].nChunks) <= 3;

    }, "never migrated", 10 * 60 * 1000, 1000);

    st.stop();

})();
