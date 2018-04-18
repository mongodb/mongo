/**
 * Test the downgrade of a standalone replica set from latest to last-stable version succeeds,
 * verifying behavior related to causal consistency at each stage.
 */
(function() {
    "use strict";

    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    var newVersion = "latest";
    var oldVersion = "last-stable";

    var name = "causal_consistency_rs_downgrade";
    var rst = new ReplSetTest(
        {name: name, nodes: 3, waitForKeys: true, nodeOptions: {binVersion: newVersion}});
    rst.startSet();
    var replSetConfig = rst.getReplSetConfig();
    // Hard-code catchup timeout to be compatible with 3.4
    replSetConfig.settings = {catchUpTimeoutMillis: 2000};
    rst.initiate(replSetConfig);

    var primary = rst.getPrimary();
    primary.getDB("test").runCommand({insert: "foo", documents: [{_id: 1, x: 1}]});
    rst.awaitReplication();

    // Nodes can accept afterClusterTime reads.
    assert.soonNoExcept(() => {
        assertContainsLogicalAndOperationTime(primary.getDB("test").runCommand({isMaster: 1}),
                                              {initialized: true, signed: false});
        return true;
    });

    rst.getSecondaries().forEach(function(secondary) {
        assert.soonNoExcept(() => {
            assertContainsLogicalAndOperationTime(secondary.getDB("test").runCommand({isMaster: 1}),
                                                  {initialized: true, signed: false});
            return true;
        });
    });

    jsTest.log("Setting FCV to 3.4 ...");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    rst.awaitReplication();

    // The system keys collection should have been dropped.
    assertHasNoKeys(rst.getPrimary());

    assertDoesNotContainLogicalOrOperationTime(
        rst.getPrimary().getDB("test").runCommand({isMaster: 1}));

    rst.getSecondaries().forEach(function(secondary) {
        assertDoesNotContainLogicalOrOperationTime(
            secondary.getDB("test").runCommand({isMaster: 1}));
    });

    jsTest.log("Downgrading secondaries ...");
    rst.upgradeSecondaries(primary, {binVersion: oldVersion});
    jsTest.log("Downgrading secondaries complete.");

    rst.getSecondaries().forEach(function(secondary) {
        assertDoesNotContainLogicalOrOperationTime(
            secondary.getDB("test").runCommand({isMaster: 1}));
    });

    jsTest.log("Downgrading primary ...");
    rst.upgradePrimary(primary, {binVersion: oldVersion});
    jsTest.log("Downgrading primary complete.");

    assertDoesNotContainLogicalOrOperationTime(
        rst.getPrimary().getDB("test").runCommand({isMaster: 1}));

    // There should still be no keys.
    assertHasNoKeys(rst.getPrimary());

    rst.stopSet();
})();
