// Tests that the keys collection is correctly handled on downgrade and re-upgrade for a standalone
// replica set.
(function() {
    "use strict";

    load("jstests/multiVersion/libs/causal_consistency_helpers.js");
    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/replsets/rslib.js");

    const lastestVersion = "latest";
    const lastStableVersion = "last-stable";

    const rst = new ReplSetTest({nodes: 3, waitForKeys: true});
    rst.startSet();
    let replSetConfig = rst.getReplSetConfig();
    // Set catchUpTimeoutMillis for compatibility with v3.4.
    replSetConfig.settings = {catchUpTimeoutMillis: 2000};
    rst.initiate(replSetConfig);

    // Verify admin.system.keys collection after startup.
    assertHasKeys(rst.getPrimary());

    // Downgrade to 3.4.
    jsTestLog("Setting FCV to 3.4.");
    assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // The system keys collection should have been dropped.
    assertHasNoKeys(rst.getPrimary());

    // Wait for the FCV document to replicate to all secondaries before downgrading them.
    rst.awaitReplication();

    jsTestLog("Downgrading binaries.");
    rst.upgradeSecondaries(rst.getPrimary(), {binVersion: lastStableVersion});
    rst.upgradePrimary(rst.getPrimary(), {binVersion: lastStableVersion});

    // Still no keys.
    assertHasNoKeys(rst.getPrimary());

    jsTestLog("Adding new node to the replica set.");
    const newNode = rst.add({binVersion: lastStableVersion, storageEngine: "wiredTiger"});
    rst.reInitiate();
    rst.awaitSecondaryNodes();
    rst.awaitNodesAgreeOnConfigVersion();

    // Upgrade back to 3.6.
    jsTestLog("Upgrading binaries.");
    rst.upgradeSecondaries(rst.getPrimary(), {binVersion: lastestVersion});
    rst.upgradePrimary(rst.getPrimary(), {binVersion: lastestVersion});

    // Still no keys.
    assertHasNoKeys(rst.getPrimary());

    jsTestLog("Setting FCV to 3.6.");
    assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "3.6"}));

    // The primary should soon create new keys.
    assert.soon(() => {
        try {
            assertHasKeys(rst.getPrimary());
            assertContainsLogicalAndOperationTime(rst.getPrimary().adminCommand({isMaster: 1}),
                                                  {initialized: true, signed: false});
        } catch (e) {
            print("Waiting for keys to be generated.");
            return false;
        }
        return true;
    });
    rst.awaitReplication();

    // Verify the admin.system.keys collection on the new node.
    const newNodeConn = new Mongo(newNode.host);
    newNodeConn.setSlaveOk(true);
    assertHasKeys(newNodeConn);
    assertContainsLogicalAndOperationTime(newNodeConn.adminCommand({isMaster: 1}),
                                          {initialized: true, signed: false});

    rst.stopSet();
})();
