/*
 * @tags: [
 *     requires_fcv_40, # autoSplitVector not present in v3.6 binaries
 * ]
 */

(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', s.shard1.shardName);

    var db = s.getDB("test");

    var bigString = "";
    while (bigString.length < 10000) {
        bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";
    }

    var inserted = 0;
    var num = 0;
    var bulk = db.foo.initializeUnorderedBulkOp();
    while (inserted < (20 * 1024 * 1024)) {
        bulk.insert({_id: num++, s: bigString});
        inserted += bigString.length;
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));
    assert.lte(20, s.config.chunks.count({"ns": "test.foo"}), "setup2");

    function diff1() {
        var x = s.chunkCounts("foo");
        printjson(x);
        return Math.max(x[s.shard0.shardName], x[s.shard1.shardName]) -
            Math.min(x[s.shard0.shardName], x[s.shard1.shardName]);
    }

    function sum() {
        var x = s.chunkCounts("foo");
        return x[s.shard0.shardName] + x[s.shard1.shardName];
    }

    assert.lte(20, diff1(), "big differential here");
    print(diff1());

    assert.soon(function() {
        var d = diff1();
        return d < 5;
        // Make sure there's enough time here, since balancing can sleep for 15s or so between
        // balances.
    }, "balance didn't happen", 1000 * 60 * 5, 5000);

    s.stop();
})();
