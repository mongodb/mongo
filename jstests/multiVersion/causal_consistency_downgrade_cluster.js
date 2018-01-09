/**
 * Test the downgrade of a sharded cluster from latest to last-stable version succeeds, verifying
 * behavior related to causal consistency at each stage.
 */
(function() {
    "use strict";

    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/multiVersion/libs/multi_cluster.js");
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Start a cluster at the latest version, with majority read concern enabled.
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {binVersion: "latest"},
            configOptions: {binVersion: "latest"},
            // Set catchUpTimeoutMillis for compatibility with v3.4.
            configReplSetTestOptions: {settings: {catchUpTimeoutMillis: 2000}},
            rsOptions: {
                binVersion: "latest",
                settings: {catchUpTimeoutMillis: 2000},
                enableMajorityReadConcern: ""
            },
            rs: true
        }
    });
    st.configRS.awaitReplication();

    st.s.getDB("test").runCommand({insert: "foo", documents: [{_id: 1, x: 1}]});

    // Both logical and operation times are returned, and cluster times are signed by mongos. Mongos
    // doesn't wait for keys at startup, so retry.
    // TODO: SERVER-31986 this check can be done only for authenticated connections that do not have
    // advance_cluster_time privilege.
    assert.soonNoExcept(function() {
        assertContainsLogicalAndOperationTime(st.s.getDB("test").runCommand({isMaster: 1}),
                                              {initialized: true, signed: false});
        return true;
    });

    // Mongos and shards can accept afterClusterTime reads.
    assertAfterClusterTimeReadSucceeds(st.s.getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadSucceeds(st.rs1.getPrimary().getDB("test"), "foo");

    // Change featureCompatibilityVersion to 3.4.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Mongos still signs cluster times, because they are held in memory.
    // TODO: SERVER-31986 this check can be done only for authenticated connections that do not have
    // advance_cluster_time privilege.
    assertContainsLogicalAndOperationTime(st.s.getDB("test").runCommand({isMaster: 1}),
                                          {initialized: true, signed: false});

    // afterClusterTime reads are no longer accepted.
    assertAfterClusterTimeReadFails(st.s.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs1.getPrimary().getDB("test"), "foo");

    // Shards and the config servers should no longer send logical or operation times.
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // Downgrade mongos first.
    jsTest.log("Downgrading mongos servers.");
    st.upgradeCluster("last-stable", {upgradeConfigs: false, upgradeShards: false});
    st.restartMongoses();

    // Mongos should no longer return operation or cluster times.
    assertDoesNotContainLogicalOrOperationTime(st.s.getDB("test").runCommand({isMaster: 1}));

    // Downgrade shards next.
    jsTest.log("Downgrading shard servers.");
    st.upgradeCluster("last-stable", {upgradeConfigs: false, upgradeMongos: false});
    st.restartMongoses();

    // Finally, downgrade config servers.
    jsTest.log("Downgrading config servers.");
    st.upgradeCluster("last-stable", {upgradeMongos: false, upgradeShards: false});
    st.restartMongoses();

    // No servers return logical or operation time.
    assertDoesNotContainLogicalOrOperationTime(st.s.getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs0.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.rs1.getPrimary().getDB("test").runCommand({isMaster: 1}));
    assertDoesNotContainLogicalOrOperationTime(
        st.configRS.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // afterClusterTime reads are still not accepted.
    assertAfterClusterTimeReadFails(st.s.getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs0.getPrimary().getDB("test"), "foo");
    assertAfterClusterTimeReadFails(st.rs1.getPrimary().getDB("test"), "foo");

    st.stop();
})();
