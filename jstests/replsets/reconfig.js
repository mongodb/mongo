/*
 * Simple test to ensure that an invalid reconfig fails, a valid one succeeds, and a reconfig won't
 * succeed without force if force is needed.
 */
(function() {
    "use strict";
    var numNodes = 5;
    var replTest = new ReplSetTest({name: 'testSet', nodes: numNodes});
    var nodes = replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();

    replTest.awaitSecondaryNodes();

    jsTestLog("Valid reconfig");
    var config = primary.getDB("local").system.replset.findOne();
    printjson(config);
    config.version++;
    config.members[nodes.indexOf(primary)].priority = 2;
    assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
    replTest.awaitReplication();

    jsTestLog("Invalid reconfig");
    config.version++;
    var badMember = {_id: numNodes, host: "localhost:12345", priority: "High"};
    config.members.push(badMember);
    var invalidConfigCode = 93;
    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 invalidConfigCode);

    jsTestLog("No force when needed.");
    config.members = config.members.slice(0, numNodes - 1);
    var secondary = replTest.getSecondary();
    config.members[nodes.indexOf(secondary)].priority = 5;
    var admin = secondary.getDB("admin");
    var forceRequiredCode = 10107;
    assert.commandFailedWithCode(admin.runCommand({replSetReconfig: config}), forceRequiredCode);

    jsTestLog("Force when appropriate");
    assert.commandWorked(admin.runCommand({replSetReconfig: config, force: true}));

    replTest.stopSet();
}());
