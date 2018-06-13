// Test that 'atClusterTime' triggers a noop write to advance the majority commit point if
// necessary.
// @tags: [requires_sharding]
(function() {
    "use strict";

    // Skip this test if running with --nojournal and WiredTiger.
    if (jsTest.options().noJournal &&
        (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
        print("Skipping test because running WiredTiger without journaling isn't a valid" +
              " replica set configuration");
        return;
    }

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

    // Propagate 'clusterTime' to shard 1. This ensures that its next write will be at time >=
    // 'clusterTime'.
    testDB1.coll1.find().itcount();

    // Attempt a snapshot read at 'clusterTime' on shard 1. Test that it performs a noop write to
    // advance its majority commit point. The snapshot read itself may fail if the noop write
    // advances the node's majority commit point past 'clusterTime' and it releases that snapshot.
    // Test reading from the primary.
    const shard1Session =
        st.rs1.getPrimary().getDB("test1").getMongo().startSession({causalConsistency: false});
    shard1Session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    res = shard1Session.getDatabase("test1").runCommand({find: "coll1"});
    assert.commandFailedWithCode(shard1Session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    if (res.ok === 0) {
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);
    }
    const shard1PrimaryMajOpTime =
        st.rs1.getReadConcernMajorityOpTimeOrThrow(st.rs1.getPrimary()).ts;
    assert.gte(shard1PrimaryMajOpTime, clusterTime);

    // Perform a write on shard 1 and get its op time.
    res = assert.commandWorked(testDB1.runCommand({insert: "coll1", documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    clusterTime = res.operationTime;

    // Propagate 'clusterTime' to shard 0. This ensures that its next write will be at time >=
    // 'clusterTime'.
    testDB0.coll0.find().readPref('secondary').itcount();

    // Attempt a snapshot read at 'clusterTime' on shard 0. Test that it performs a noop write to
    // advance its majority commit point. The snapshot read itself may fail if the noop write
    // advances the node's majority commit point past 'clusterTime' and it releases that snapshot.
    // Test reading from the secondary.
    const shard0Session =
        st.rs0.getSecondary().getDB("test0").getMongo().startSession({causalConsistency: false});
    shard0Session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    res = shard0Session.getDatabase("test0").runCommand({find: "coll0"});
    assert.commandFailedWithCode(shard0Session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    if (res.ok === 0) {
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);
    }
    const shard0SecondaryMajOpTime =
        st.rs0.getReadConcernMajorityOpTimeOrThrow(st.rs0.getSecondary()).ts;
    assert.gte(shard0SecondaryMajOpTime, clusterTime);

    st.stop();
}());
