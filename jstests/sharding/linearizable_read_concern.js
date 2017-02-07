/*
 * This test exercises the "linearizable" readConcern option on a simple sharded cluster.
 * Note that a full linearizable read concern test exists in
 * "replsets/linearizable_read_concern.js". This test exists mainly to affirm that a
 * readConcern "linearizable" propagates correctly through a sharded cluster i.e. we
 * execute database commands only through a mongos, not directly on a replica set.
 *
 * There is one mongos and two shards (each a 3-node replica set). We put one
 * chunk on each shard, each containing five documents. We then execute a
 * linearizable read targeting both shards with readPreference "secondary", to
 * make sure it fails. We next execute a linearizable read targeting both
 * shards with readPreference "primary" to make sure it succeeds. The primary
 * is then partitioned from the other two secondaries in the first shard, and
 * we make sure that a linearizable read targeting primaries in both shards
 * times out, since the partitioned primary can no longer communicate with a
 * majority of nodes.
 *
 * NOTE: Linearizability guarantees only apply when a query specifies a unique
 * document. This test is mainly trying to ensure that system behavior is
 * reasonable when executing linearizable reads in a sharded cluster, so as to
 * exercise possible (invalid) user behavior.
 */

load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");

(function() {
    "use strict";

    var testName = "linearizable_read_concern";

    var st = new ShardingTest({
        name: testName,
        shards: 2,
        other: {rs0: {nodes: 3}, rs1: {nodes: 3}, useBridge: true},
        mongos: 1,
        config: 1,
        enableBalancer: false
    });

    jsTestLog("Setting up sharded cluster.");

    // Set up the sharded cluster.
    var dbName = testName;
    var collName = "test";
    var collNamespace = dbName + "." + collName;
    var shard0ReplTest = st.rs0;
    var testDB = st.s.getDB(dbName);

    // Set high election timeout so that primary doesn't step down during linearizable read test.
    var cfg = shard0ReplTest.getReplSetConfigFromNode(0);
    cfg.settings.electionTimeoutMillis = shard0ReplTest.kDefaultTimeoutMS;
    reconfig(shard0ReplTest, cfg, true);

    // Set up sharded collection. Put 5 documents on each shard, with keys {x: 0...9}.
    var numDocs = 10;
    shardCollectionWithChunks(st, testDB[collName], numDocs);

    // Print current sharding stats for debugging.
    st.printShardingStatus(5);

    // Filter to target one document in each shard.
    var shard0DocKey = 2;
    var shard1DocKey = 7;
    var dualShardQueryFilter = {$or: [{x: shard0DocKey}, {x: shard1DocKey}]};

    jsTestLog("Testing linearizable read from secondaries");

    // Execute a linearizable read from secondaries (targeting both shards) which should fail.
    st.s.setReadPref("secondary");
    var res = assert.commandFailed(testDB.runReadCommand({
        find: collName,
        filter: dualShardQueryFilter,
        readConcern: {level: "linearizable"},
        maxTimeMS: shard0ReplTest.kDefaultTimeoutMS
    }));
    assert.eq(res.code, ErrorCodes.NotMaster);

    jsTestLog("Testing linearizable read from primaries.");

    // Execute a linearizable read from primaries (targeting both shards) which should succeed.
    st.s.setReadPref("primary");
    var res = assert.writeOK(testDB.runReadCommand({
        find: collName,
        sort: {x: 1},
        filter: dualShardQueryFilter,
        readConcern: {level: "linearizable"},
        maxTimeMS: shard0ReplTest.kDefaultTimeoutMS
    }));

    // Make sure data was returned from both shards correctly.
    assert.eq(res.cursor.firstBatch[0].x, shard0DocKey);
    assert.eq(res.cursor.firstBatch[1].x, shard1DocKey);

    jsTestLog("Testing linearizable read targeting partitioned primary.");

    var primary = shard0ReplTest.getPrimary();
    var secondaries = shard0ReplTest.getSecondaries();

    // Partition the primary in the first shard.
    secondaries[0].disconnect(primary);
    secondaries[1].disconnect(primary);

    jsTestLog("Current Replica Set Topology of First Shard: [Secondary-Secondary] [Primary]");

    // Execute a linearizable read targeting the partitioned primary in first shard, and good
    // primary in the second shard. This should time out due to partitioned primary.
    var result = testDB.runReadCommand({
        find: collName,
        filter: dualShardQueryFilter,
        readConcern: {level: "linearizable"},
        maxTimeMS: 3000
    });
    assert.commandFailedWithCode(result, ErrorCodes.ExceededTimeLimit);

    st.stop();
})();
