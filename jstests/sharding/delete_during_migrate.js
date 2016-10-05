// Test migrating a big chunk while deletions are happening within that chunk. Test is slightly
// non-deterministic, since removes could happen before migrate starts. Protect against that by
// making chunk very large.
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var dbname = "test";
    var coll = "foo";
    var ns = dbname + "." + coll;

    assert.commandWorked(st.s0.adminCommand({enablesharding: dbname}));
    st.ensurePrimaryShard(dbname, 'shard0001');

    var t = st.s0.getDB(dbname).getCollection(coll);

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < 200000; i++) {
        bulk.insert({a: i});
    }
    assert.writeOK(bulk.execute());

    // enable sharding of the collection. Only 1 chunk.
    t.ensureIndex({a: 1});

    assert.commandWorked(st.s0.adminCommand({shardcollection: ns, key: {a: 1}}));

    // start a parallel shell that deletes things
    var join = startParallelShell("db." + coll + ".remove({});", st.s0.port);

    // migrate while deletions are happening
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: ns, find: {a: 1}, to: st.getOther(st.getPrimaryShard(dbname)).name}));

    join();

    st.stop();
})();
