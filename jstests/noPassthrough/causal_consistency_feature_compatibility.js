/**
 * Tests the behavior of a sharded cluster when featureCompatibilityVersion is set to 3.4, with
 * respect to causal consistency. In noPassthrough to avoid issues with auth.
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    function logicalTimeCanBeProcessed(db) {
        const increment = 5000;

        let initialTime = db.runCommand({isMaster: 1}).$clusterTime;
        if (!initialTime) {
            return false;
        }

        let laterTime = Object.merge(
            initialTime,
            {clusterTime: Timestamp(initialTime.clusterTime.getTime() + increment, 0)});
        let returnedTime = rst.getPrimary()
                               .getDB("test")
                               .runCommand({isMaster: 1, $clusterTime: laterTime})
                               .$clusterTime;

        // Use a range to allow for unrelated activity advancing cluster time.
        return (returnedTime.clusterTime.getTime() - initialTime.clusterTime.getTime()) >=
            increment;
    }

    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            enableMajorityReadConcern: "",
            shardsvr: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }
    rst.initiate();

    // Start the sharding test and add the majority read concern enabled replica set.
    const st = new ShardingTest({manualAddShard: true, mongosWaitsForKeys: true});
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));

    const testDB = st.s.getDB("test");

    // Initialize sharding.
    assert.commandWorked(testDB.adminCommand({enableSharding: "test"}));
    assert.commandWorked(
        testDB.adminCommand({shardCollection: testDB.foo.getFullName(), key: {_id: 1}}));

    // Insert some data to find.
    assert.commandWorked(testDB.runCommand(
        {insert: "foo", documents: [{_id: 1, x: 1}], writeConcern: {w: "majority"}}));

    // Verify afterClusterTime can be processed by mongos and mongod.
    assert.commandWorked(testDB.runCommand(
        {find: "foo", readConcern: {level: "majority", afterClusterTime: Timestamp(1, 1)}}));

    assert.commandWorked(rst.getPrimary().getDB("test").runCommand(
        {find: "foo", readConcern: {level: "majority", afterClusterTime: Timestamp(1, 1)}}));

    // Verify cluster time can be processed by shards and the config servers.
    assert(logicalTimeCanBeProcessed(rst.getPrimary().getDB("test")));
    assert(logicalTimeCanBeProcessed(st.configRS.getPrimary().getDB("test")));

    // Set featureCompatibilityVersion to 3.4
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Verify cluster time cannot be processed by shards and the config servers now.
    assert(!logicalTimeCanBeProcessed(rst.getPrimary().getDB("test")));
    assert(!logicalTimeCanBeProcessed(st.configRS.getPrimary().getDB("test")));

    // afterClusterTime should be rejected by mongos and mongod.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {find: "foo", readConcern: {level: "majority", afterClusterTime: Timestamp(1, 1)}}),
        ErrorCodes.InvalidOptions);

    assert.commandFailedWithCode(
        rst.getPrimary().getDB("test").runCommand(
            {find: "foo", readConcern: {level: "majority", afterClusterTime: Timestamp(1, 1)}}),
        ErrorCodes.InvalidOptions);

    // setFeatureCompatibilityVersion can only be run on the admin database on mongos.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Verify cluster time can be processed by shards and the config servers again.
    assert(logicalTimeCanBeProcessed(rst.getPrimary().getDB("test")));
    assert(logicalTimeCanBeProcessed(st.configRS.getPrimary().getDB("test")));

    st.stop();
})();
