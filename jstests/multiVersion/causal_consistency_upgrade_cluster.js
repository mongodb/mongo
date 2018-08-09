/**
 * Tests upgrading a cluster with two shards and two mongos servers from last stable to current
 * version, verifying the behavior of $clusterTime metadata and afterClusterTime commands throughout
 * the process.
 * @tags: [requires_majority_read_concern]
 */
(function() {
    "use strict";

    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/libs/sessions_collection.js");
    load("jstests/multiVersion/libs/multi_cluster.js");
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Start a cluster at the last stable version, with majority read concern enabled.
    var st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            mongosOptions: {binVersion: "last-stable"},
            configOptions: {binVersion: "last-stable"},
            rsOptions: {binVersion: "last-stable", enableMajorityReadConcern: ""},
            rs: true
        }
    });
    st.configRS.awaitReplication();

    st.s.getDB("test").runCommand({insert: "foo", documents: [{_id: 1, x: 1}]});

    // No servers return logical or operation time.
    assertDoesNotContainLogicalOrOperationTime(st.s0.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(st.s1.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // Upgrade the config servers.
    jsTest.log("Upgrading config servers.");
    st.upgradeCluster("latest", {upgradeMongos: false, upgradeShards: false});
    st.restartMongoses();

    // Mongod and mongos cannot accept afterClusterTime reads.
    assertAfterClusterTimeReadFails(st.s0.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.s1.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs1.getPrimary().getDB("test"), "foo");

    // Config servers still don't return logical or operation times.
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    assertDoesNotContainLogicalOrOperationTime(st.s0.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(st.s1.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // Then upgrade the shard servers.
    jsTest.log("Upgrading shard servers.");
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: false});
    st.restartMongoses();

    // Mongod and mongos still cannot accept afterClusterTime reads.
    assertAfterClusterTimeReadFails(st.s0.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.s1.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs1.getPrimary().getDB("test"), "foo");

    // Shards still don't return logical or operation times.
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // Neither do config servers.
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    assertDoesNotContainLogicalOrOperationTime(st.s0.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(st.s1.getDB("test").runCommand({isMaster: 1}));

    // Finally, upgrade mongos servers.
    jsTest.log("Upgrading mongos servers.");
    st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});
    st.restartMongoses();

    // afterClusterTime reads are still not accepted.
    assertAfterClusterTimeReadFails(st.s.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs1.getPrimary().getDB("test"), "foo");

    // Neither mongos returns cluster time or operation time, because there are no keys in the
    // config server, since feature compatibility version is still 3.4.
    assertDoesNotContainLogicalOrOperationTime(st.s0.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(st.s1.getDB("test").runCommand({isMaster: 1}));

    // All shards and the config servers still don't return logical or operation time.
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // Set feature compatibility version to 3.6 on one mongos.
    assert.commandWorked(st.s0.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Now shards and config servers return dummy signed cluster times and operation times.
    assertContainsLogicalAndOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}),
        {initialized: true, signed: false});
    assertContainsLogicalAndOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}),
        {initialized: true, signed: false});
    assertContainsLogicalAndOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}),
        {initialized: true, signed: false});

    // Once the config primary creates keys, both mongos servers discover them and start returning
    // signed cluster times.
    // TODO: SERVER-31986 this check can be done only for authenticated connections that do not have
    // advance_cluster_time privilege.
    assert.soonNoExcept(function() {
        assertContainsLogicalAndOperationTime(st.s0.getDB("test").runCommand({isMaster: 1}),
                                              {initialized: true, signed: false});
        assertContainsLogicalAndOperationTime(st.s1.getDB("test").runCommand({isMaster: 1}),
                                              {initialized: true, signed: false});
        return true;
    });

    // Now shards and mongos can accept afterClusterTime reads.
    assertAfterClusterTimeReadSucceeds(st.s0.getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.s1.getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.rs1.getPrimary().getDB("test"), "foo");

    // Test that restarted mongoses are able to connect after FCV update.
    st.restartMongoses();
    assertAfterClusterTimeReadSucceeds(st.s0.getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.s1.getDB("test"), "foo");

    // Causally consistent requests are correctly processed.
    let res = assert.commandWorked(
        st.s.getDB("test").runCommand({insert: "foo", documents: [{_id: 2, x: 2}]}));
    res = assert.commandWorked(
        st.s.getDB("test").runCommand({delete: "foo", deletes: [{q: {_id: 1}, limit: 1}]}));

    let operationTime = res.operationTime;
    res = assert.commandWorked(st.s.getDB("test").runCommand(
        {find: "foo", readConcern: {level: "majority", afterClusterTime: operationTime}}));
    assert.eq(res.cursor.firstBatch, [{_id: 2, x: 2}]);

    // force config server to create sessions collection
    assert.commandWorked(
        st.configRS.getPrimary().getDB('admin').runCommand({refreshLogicalSessionCacheNow: 1}));
    // system.sessions collection can never be on config server.
    validateSessionsCollection(st.configRS.getPrimary(), false, false, true);
    // The system sessions collection should have been created on some shard.
    assert(validateSessionsCollection(st.rs0.getPrimary(), true, true, false) ||
           validateSessionsCollection(st.rs1.getPrimary(), true, true, false));

    st.stop();
})();
