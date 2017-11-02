// This tests that the keys collection is correctly handled on downgrade and re-upgrade.
//
// In fcv 3.6, the CSRS generates keys for causal consistency in admin.system.keys. Replication only
// replicates system collections that are in its whitelist. So if a new node were added to the CSRS
// after downgrade, the keys collection would not replicate to the new node because it has a v3.4
// whitelist without admin.system.keys. On re-upgrade, that node would be missing the keys.
// Therefore, here we test that the collection is dropped as expected on downgrade and avoids the
// re-upgrade inconsistencies.
//
// TODO: this test should be removed in v3.7 when it's no longer relevant.

(function() {
    "use strict";

    load("jstests/multiVersion/libs/multi_cluster.js");
    load("jstests/replsets/rslib.js");

    let st = new ShardingTest({
        shards: {},
        mongos: 1,
        mongosWaitsForKeys: true,
        other: {
            // Set catchUpTimeoutMillis for compatibility with v3.4.
            configReplSetTestOptions: {settings: {catchUpTimeoutMillis: 2000}},
            rsOptions: {settings: {catchUpTimeoutMillis: 2000}}
        }
    });
    let keysCollectionName = "admin.system.keys";

    jsTestLog("Verify the admin.system.keys collection after startup.");
    st.configRS.awaitReplication();
    let keyCount = st.configRS.getPrimary().getCollection(keysCollectionName).find().count();
    assert.gte(keyCount, 2, "There should be admin.system.keys entries!! Found: " + keyCount);

    jsTestLog("Set feature compatibility version to 3.4.");
    assert.commandWorked(st.s0.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    jsTestLog("Verify the admin.system.keys collection is gone.");
    keyCount = st.configRS.getPrimary().getCollection('admin.system.keys').find().count();
    assert.eq(keyCount, 0, "Expected no keys on downgrade to fcv 3.4");

    jsTest.log("Downgrading mongos servers to binary v3.4.");
    st.upgradeCluster("last-stable", {upgradeConfigs: false, upgradeShards: false});
    st.restartMongoses();

    jsTest.log("Downgrading shard servers to binary v3.4.");
    st.upgradeCluster("last-stable", {upgradeConfigs: false, upgradeMongos: false});
    st.restartMongoses();

    jsTest.log("Downgrading config servers to binary v3.4.");
    st.upgradeCluster("last-stable", {upgradeMongos: false, upgradeShards: false});
    st.configRS.awaitNodesAgreeOnPrimary();
    st.restartMongoses();

    jsTest.log("Add new node to CSRS.");
    let newConfigNode =
        st.configRS.add({binVersion: 'last-stable', configsvr: "", storageEngine: "wiredTiger"});
    st.configRS.reInitiate();
    st.configRS.awaitSecondaryNodes();
    st.configRS.awaitNodesAgreeOnConfigVersion();

    jsTest.log("Upgrade cluster to binary v3.6.");
    st.upgradeCluster("latest");

    jsTestLog("Set feature compatibility version to 3.6.");
    assert.commandWorked(st.s0.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    st.restartMongoses();

    jsTestLog("Verify the admin.system.keys collection on the newer CSRS node.");
    let node = new Mongo(newConfigNode.host);
    node.setSlaveOk(true);
    let newNodeKeyCount = node.getCollection(keysCollectionName).find().count();

    assert.gte(newNodeKeyCount,
               2,
               "There should be admin.system.keys entries on upgrade to fcv 3.6! Found: " +
                   newNodeKeyCount);

    jsTest.log('DONE!');
    st.stop();

})();
