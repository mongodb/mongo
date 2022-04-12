/**
 * Tests that cluster server parameter feature flags work correctly.
 *
 * TODO: Delete this test once we branch for 6.0.
 *
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
"use strict";

load("jstests/multiVersion/libs/verify_versions.js");
load("jstests/multiVersion/libs/multi_rs.js");       // For upgradeSecondaries and upgradeSet.
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

const kDowngradeVersion = "last-lts";
const kUpgradeVersion = "latest";

function assertSetClusterParameterFailsAndthenSucceedsAfterFCVUpgrade(db) {
    // Despite an upgrade, the test shouldn't pass because the FCV has not been explicitly set.
    assert.commandFailedWithCode(
        db.runCommand({setClusterParameter: {testStrClusterParameter: {strData: "ok"}}}),
        ErrorCodes.IllegalOperation);

    // Set the FCV; the test should now pass.
    assert.commandWorked(
        db.runCommand({setFeatureCompatibilityVersion: binVersionToFCV(kUpgradeVersion)}));
    assert.commandWorked(
        db.runCommand({setClusterParameter: {testStrClusterParameter: {strData: "ok"}}}));
}

function assertSetClusterParameterFailsInDowngradedVersion(db) {
    assert.commandFailedWithCode(
        db.runCommand({setClusterParameter: {testStrClusterParameter: {strData: "ok"}}}),
        ErrorCodes.CommandNotFound);
}

function replicaSetClusterParameterIsFCVGated() {
    const dbName = "admin";

    // Set up a replica-set in a 'downgraded' version.
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: kDowngradeVersion}});
    rst.startSet();
    rst.initiate();

    assertSetClusterParameterFailsInDowngradedVersion(rst.getPrimary().getDB(dbName));

    // Upgrade the replica set.
    rst.upgradeSet({binVersion: kUpgradeVersion});

    // Verify that all nodes are in the latest version.
    for (const node of rst.nodes) {
        assert.binVersion(node, kUpgradeVersion);
    }

    rst.awaitNodesAgreeOnPrimary();

    assertSetClusterParameterFailsAndthenSucceedsAfterFCVUpgrade(rst.getPrimary().getDB(dbName));

    rst.stopSet();
}

function shardedClusterParameterIsFCVGated() {
    const dbName = "admin";

    // Set up a sharded cluster in a 'downgraded' version.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2, binVersion: kDowngradeVersion},
        other: {
            mongosOptions: {binVersion: kDowngradeVersion},
            configOptions: {binVersion: kDowngradeVersion}
        }
    });

    // Sanity check: setClusterParameter shouldn't pass in 'downgraded' version.
    assertSetClusterParameterFailsInDowngradedVersion(st.s.getDB(dbName));

    // Upgrade the cluster.
    st.upgradeCluster(kUpgradeVersion, {waitUntilStable: true});

    assertSetClusterParameterFailsAndthenSucceedsAfterFCVUpgrade(st.s.getDB(dbName));

    st.stop();
}

replicaSetClusterParameterIsFCVGated();
shardedClusterParameterIsFCVGated();
})();
