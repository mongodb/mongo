/*
 * Test that arbiters vote no in elections if they can see a healthy primary of equal or greater
 * priority to the candidate, preventing flapping during certain kinds of network partitions.
 *
 * 1.  Initiate a 3-node replica set with one arbiter (PSA) and a higher priority primary.
 * 2.  Create a network partition between the primary and secondary.
 * 3.  Wait long enough for the secondary to call for an election.
 * 4.  Verify the primary and secondary did not change.
 * 5.  Heal the partition.
 * 6.  Verify the primary and secondary did not change and are in the initial term.
 */

(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    var name = "no_flapping_during_network_partition";

    var replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
    var nodes = replTest.startSet();
    var config = replTest.getReplSetConfig();
    config.members[0].priority = 5;
    config.members[2].arbiterOnly = true;
    config.settings = {electionTimeoutMillis: 2000};
    replTest.initiate(config);

    function getTerm(node) {
        return node.adminCommand({replSetGetStatus: 1}).term;
    }

    replTest.waitForState(nodes[0], ReplSetTest.State.PRIMARY);

    var primary = replTest.getPrimary();
    var secondary = replTest.getSecondary();
    var initialTerm = getTerm(primary);

    jsTestLog("Create a network partition between the primary and secondary.");
    primary.disconnect(secondary);

    jsTestLog("Wait long enough for the secondary to call for an election.");
    checkLog.contains(secondary, "can see a healthy primary");

    jsTestLog("Verify the primary and secondary do not change during the partition.");
    assert.eq(primary, replTest.getPrimary());
    assert.eq(secondary, replTest.getSecondary());

    jsTestLog("Heal the partition.");
    primary.reconnect(secondary);

    jsTestLog("Verify the primary and secondary did not change and are in the initial term.");
    assert.eq(primary, replTest.getPrimary());
    assert.eq(secondary, replTest.getSecondary());
    assert.eq(initialTerm, getTerm(primary));
    assert.eq(initialTerm, getTerm(secondary));

    replTest.stopSet();
})();
