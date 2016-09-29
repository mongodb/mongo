/**
 * This test confirms that chunks get split as they grow due to data insertion.
 */
(function() {
    'use strict';

    var s = new ShardingTest({name: "auto1", shards: 2, mongos: 1});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

    var bigString = "";
    while (bigString.length < 1024 * 50)
        bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

    var db = s.getDB("test");
    var coll = db.foo;

    var i = 0;

    var bulk = coll.initializeUnorderedBulkOp();
    for (; i < 100; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    var primary = s.getPrimaryShard("test").getDB("test");

    var counts = [];

    s.printChunks();
    counts.push(s.config.chunks.count());
    assert.eq(100, db.foo.find().itcount());

    print("datasize: " +
          tojson(s.getPrimaryShard("test").getDB("admin").runCommand({datasize: "test.foo"})));

    bulk = coll.initializeUnorderedBulkOp();
    for (; i < 200; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    s.printChunks();
    s.printChangeLog();
    counts.push(s.config.chunks.count());

    bulk = coll.initializeUnorderedBulkOp();
    for (; i < 400; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    s.printChunks();
    s.printChangeLog();
    counts.push(s.config.chunks.count());

    bulk = coll.initializeUnorderedBulkOp();
    for (; i < 700; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    s.printChunks();
    s.printChangeLog();
    counts.push(s.config.chunks.count());

    assert(counts[counts.length - 1] > counts[0], "counts 1 : " + tojson(counts));
    var sorted = counts.slice(0);
    // Sort doesn't sort numbers correctly by default, resulting in fail
    sorted.sort(function(a, b) {
        return a - b;
    });
    assert.eq(counts, sorted, "counts 2 : " + tojson(counts));

    print(counts);

    printjson(db.stats());

    s.stop();
})();
