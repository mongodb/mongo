/*
 * Sanity check that get_status works as expected. There are C++ unit tests for most of the
 * functionality, so we'll just check that it succeeds and fails when it's supposed to.
 */

(function() {
    "use strict";
    var name = "getstatus";
    var numNodes = 4;
    var replTest = new ReplSetTest({name: name, nodes: numNodes});
    var nodes = replTest.startSet();

    var config = replTest.getReplSetConfig();
    config.members[numNodes - 1].arbiterOnly = true;
    // An invalid time to get status
    var statusBeforeInitCode = 94;
    assert.commandFailedWithCode(nodes[0].getDB("admin").runCommand({replSetGetStatus: 1}),
                                 statusBeforeInitCode,
                                 "replSetGetStatus should fail before initializing.");
    replTest.initiate(config);
    replTest.awaitSecondaryNodes();

    // A valid status
    var primary = replTest.getPrimary();
    assert.commandWorked(primary.getDB("admin").runCommand({replSetGetStatus: 1}));

    replTest.stopSet();
}());
