/**
 * Tests that a mongod on the latest version will refuse to sync from a 3.0 node.
 */
(function() {
    "use strict";

    //
    // Test that the latest version cannot replicate from a 3.0 node.
    //
    var rst = new ReplSetTest({nodes: [{binVersion: "3.0"}, {binVersion: "latest"}]});
    rst.startSet();

    // Make sure the 3.0 node will be elected.
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[0].priority = 1;
    replSetConfig.members[1].priority = 0;
    var threeZeroAdminDB = rst.nodes[0].getDB("admin");
    var latestAdminDB = rst.nodes[1].getDB("admin");
    assert.commandWorked(threeZeroAdminDB.runCommand({replSetInitiate: replSetConfig}));
    assert.soon(
        function() {
            try {
                latestAdminDB.runCommand({ping: 1});
            } catch (e) {
                return true;
            }
            return false;
        },
        "Expected latest node to terminate when communicating to node which does not support find" +
            " commands, but it didn't.");

    rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});

    //
    // Test that a 3.0 node cannot replicate off the latest node if that node is launched with the
    // default featureCompatibilityVersion.
    //
    rst = new ReplSetTest({nodes: [{binVersion: "latest"}]});
    rst.startSet();

    // Configure the set to use protocol version 0 so that a 3.0 node can participate.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.protocolVersion = 0;
    rst.initiate(replSetConfig);

    // Add the node, but don't wait for it to start up, since we don't expect it to ever get to
    // state SECONDARY.
    rst.add({binVersion: "3.0"});

    // Rig the election so that the first node running latest version remains the primary after the
    // 3.0 secondary is added to the replica set.
    replSetConfig = rst.getReplSetConfig();
    replSetConfig.version = 2;
    replSetConfig.members[1].priority = 0;

    assert.commandWorked(rst.getPrimary().adminCommand({replSetReconfig: replSetConfig}));

    // Verify that the 3.0 node cannot participate in the set. It should eventually exceed the
    // maximum number of retries for initial sync and fail.
    rst.waitForState(rst.nodes, [ReplSetTest.State.SECONDARY, ReplSetTest.State.DOWN]);
    rst.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});
})();
