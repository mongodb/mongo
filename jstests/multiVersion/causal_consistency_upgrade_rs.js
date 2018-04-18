/**
 * Test the upgrade of a standalone replica set from the last-stable to the version succeeds,
 * verifying behavior related to causal consistency at each stage.
 */
(function() {
    "use strict";

    load("jstests/multiVersion/libs/multi_rs.js");
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    var newVersion = "latest";
    var oldVersion = "last-stable";

    var name = "causal_consistency_rs_upgrade";
    var rst = new ReplSetTest(
        {name: name, nodes: 3, nodeOptions: {binVersion: oldVersion}, waitForKeys: false});
    rst.startSet();
    var replSetConfig = rst.getReplSetConfig();
    rst.initiate(replSetConfig);

    var primary = rst.getPrimary();
    primary.getDB("test").runCommand({insert: "foo", documents: [{_id: 1, x: 1}]});
    rst.awaitReplication();

    assertDoesNotContainLogicalOrOperationTime(primary.getDB("test").runCommand({isMaster: 1}));

    rst.getSecondaries().forEach(function(secondary) {
        assertDoesNotContainLogicalOrOperationTime(
            secondary.getDB("test").runCommand({isMaster: 1}));
    });

    jsTest.log("Upgrading secondaries ...");
    rst.upgradeSecondaries(primary, {binVersion: newVersion});
    jsTest.log("Upgrading secondaries complete.");

    rst.getSecondaries().forEach(function(secondary) {
        assertDoesNotContainLogicalOrOperationTime(
            secondary.getDB("test").runCommand({isMaster: 1}));
    });

    jsTest.log("Upgrading primary ...");
    rst.upgradePrimary(primary, {binVersion: newVersion});
    jsTest.log("Upgrading primary complete.");

    assertDoesNotContainLogicalOrOperationTime(
        rst.getPrimary().getDB("test").runCommand({isMaster: 1}));

    rst.getSecondaries().forEach(function(secondary) {
        assertDoesNotContainLogicalOrOperationTime(
            secondary.getDB("test").runCommand({isMaster: 1}));
    });

    jsTest.log("Setting FCV to 3.6 ...");

    assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    rst.awaitReplication();

    rst.getPrimary().getDB("test").runCommand({insert: "foo", documents: [{_id: 2, x: 1}]});
    rst.awaitReplication();

    assert.soonNoExcept(() => {
        assertContainsLogicalAndOperationTime(
            rst.getPrimary().getDB("test").runCommand({isMaster: 1}),
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

    rst.stopSet();
})();
