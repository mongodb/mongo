// This test fails when run with authentication because benchRun with auth is broken: SERVER-6388
(function() {
    "use strict";

    var numShards = 3;
    var s = new ShardingTest({name: "parallel", shards: numShards, mongos: 2});

    s.adminCommand({enablesharding: "test"});
    s.ensurePrimaryShard('test', 'shard0001');
    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    var db = s.getDB("test");

    var N = 10000;

    for (var i = 0; i < N; i += (N / 12)) {
        s.adminCommand({split: "test.foo", middle: {_id: i}});
        s.s.getDB('admin').runCommand({
            moveChunk: "test.foo",
            find: {_id: i},
            to: "shard000" + Math.floor(Math.random() * numShards)
        });
    }

    s.startBalancer();

    var bulk = db.foo.initializeUnorderedBulkOp();
    for (i = 0; i < N; i++)
        bulk.insert({_id: i});
    assert.writeOK(bulk.execute());

    var doCommand = function(dbname, cmd) {
        x = benchRun({
            ops: [{op: "findOne", ns: dbname + ".$cmd", query: cmd}],
            host: db.getMongo().host,
            parallel: 2,
            seconds: 2
        });
        printjson(x);
        x = benchRun({
            ops: [{op: "findOne", ns: dbname + ".$cmd", query: cmd}],
            host: s._mongos[1].host,
            parallel: 2,
            seconds: 2
        });
        printjson(x);
    };

    doCommand("test", {dbstats: 1});
    doCommand("config", {dbstats: 1});

    var x = s.getDB("config").stats();
    assert(x.ok, tojson(x));
    printjson(x);

    s.stop();
}());