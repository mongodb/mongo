// Test that 'atClusterTime' triggers a noop write to advance the cluster time if necessary.
(function() {
    "use strict";

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");
    if (!assert.commandWorked(conn.getDB("test").serverStatus())
             .storageEngine.supportsSnapshotReadConcern) {
        MongoRunner.stopMongod(conn);
        return;
    }
    MongoRunner.stopMongod(conn);

    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

    // Create database "test0" on shard 0.
    const testDB0 = st.s.getDB("test0");
    assert.commandWorked(testDB0.adminCommand({enableSharding: testDB0.getName()}));
    st.ensurePrimaryShard(testDB0.getName(), st.shard0.shardName);
    assert.commandWorked(testDB0.createCollection("coll0"));

    // Create a database "test1" on shard 1.
    const testDB1 = st.s.getDB("test1");
    assert.commandWorked(testDB1.adminCommand({enableSharding: testDB1.getName()}));
    st.ensurePrimaryShard(testDB1.getName(), st.shard1.shardName);
    assert.commandWorked(testDB1.createCollection("coll1"));

    // Perform a write on shard 0 and get its op time.
    let res = assert.commandWorked(testDB0.runCommand({insert: "coll0", documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    let clusterTime = res.operationTime;

    // It should be possible to read from the latest cluster time on shard 1 because it performs a
    // noop write to advance its op time. Test reading from the primary.
    const shard1Session =
        st.rs1.getPrimary().getDB("test1").getMongo().startSession({causalConsistency: false});
    assert.commandWorked(shard1Session.getDatabase("test1").runCommand({
        find: "coll1",
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
        txnNumber: NumberLong(0)
    }));

    // Perform a write on shard 1 and get its op time.
    res = assert.commandWorked(testDB1.runCommand({insert: "coll1", documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    clusterTime = res.operationTime;

    // It should be possible to read from the latest cluster time on shard 0 because it performs a
    // noop write to advance its op time. Test reading from the secondary.
    const shard0Session =
        st.rs0.getSecondary().getDB("test0").getMongo().startSession({causalConsistency: false});
    assert.commandWorked(shard0Session.getDatabase("test0").runCommand({
        find: "coll0",
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
        txnNumber: NumberLong(0)
    }));

    st.stop();
}());
