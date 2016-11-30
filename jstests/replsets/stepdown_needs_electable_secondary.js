/**
 * Test to ensure that replSetStepDown called on a primary will only succeed if a majority of nodes
 * are caught up to it and that at least one node in this majority is electable. Tests this with a
 * 5 node replica set.
 *
 * 1.  Initiate a 5-node replica set
 * 2.  Disable replication to all secondaries
 * 3.  Execute some writes on primary
 * 4.  Try to step down primary and expect to fail
 * 5.  Enable replication to one unelectable secondary, secondary B
 * 6.  Await replication to secondary B by executing primary write with writeConcern:2
 * 7.  Try to step down primary and expect failure
 * 8.  Enable replication to a different unelectable secondary, secondary C
 * 9.  Await replication to secondary C by executing primary write with writeConcern:3
 * 10. Try to step down primary and expect failure
 * 11. Enable replication to an electable secondary, secondary A
 * 12. Await replication to secondary A by executing primary write with writeConcern:4
 * 13. Try to step down primary and expect success
 * 14. Assert that original primary is now a secondary
 *
 */
(function() {
    load("jstests/libs/write_concern_util.js");  // for stopReplicationOnSecondaries,
                                                 // restartServerReplication,
                                                 // restartReplSetReplication

    'use strict';

    var name = 'stepdown_needs_electable_secondary';

    var replTest = new ReplSetTest({name: name, nodes: 5});
    var nodes = replTest.nodeList();

    replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2]},
            {"_id": 3, "host": nodes[3], "priority": 0},  // unelectable
            {"_id": 4, "host": nodes[4], "priority": 0}   // unelectable
        ],
        "settings": {"chainingAllowed": false}
    });

    function assertStepDownFailsWithExceededTimeLimit(node) {
        assert.commandFailedWithCode(
            node.getDB("admin").runCommand({replSetStepDown: 5, secondaryCatchUpPeriodSecs: 5}),
            ErrorCodes.ExceededTimeLimit,
            "step down did not fail with 'ExceededTimeLimit'");
    }

    function assertStepDownSucceeds(node) {
        assert.throws(function() {
            node.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60});
        });
    }

    var primary = replTest.getPrimary();

    jsTestLog("Blocking writes to all secondaries.");
    stopReplicationOnSecondaries(replTest);

    jsTestLog("Doing a write to primary.");
    var testDB = replTest.getPrimary().getDB('testdb');
    var coll = testDB.stepdown_needs_electable_secondary;
    var timeout = 5 * 60 * 1000;
    assert.writeOK(
        coll.insert({"dummy_key": "dummy_val"}, {writeConcern: {w: 1, wtimeout: timeout}}));

    // Try to step down with only the primary caught up (1 node out of 5).
    // stepDown should fail.
    jsTestLog("Trying to step down primary with only 1 node out of 5 caught up.");
    assertStepDownFailsWithExceededTimeLimit(primary);

    // Get the two unelectable secondaries
    var secondaryB_unelectable = replTest.nodes[3];
    var secondaryC_unelectable = replTest.nodes[4];

    // Get an electable secondary
    var secondaryA_electable = replTest.getSecondaries().find(function(s) {
        var nodeId = replTest.getNodeId(s);
        return (nodeId !== 3 && nodeId !== 4);  // nodes 3 and 4 are set to be unelectable
    });

    // Enable writes to Secondary B (unelectable). Await replication.
    // (2 out of 5 nodes caught up, 0 electable)
    // stepDown should fail due to no caught up majority.
    jsTestLog("Re-enabling writes to unelectable secondary: node #" +
              replTest.getNodeId(secondaryB_unelectable) + ", " + secondaryB_unelectable);
    restartServerReplication(secondaryB_unelectable);

    // Wait for this secondary to catch up by issuing a write that must be replicated to 2 nodes
    assert.writeOK(
        coll.insert({"dummy_key": "dummy_val"}, {writeConcern: {w: 2, wtimeout: timeout}}));

    // Try to step down and fail
    jsTestLog("Trying to step down primary with only 2 nodes out of 5 caught up.");
    assertStepDownFailsWithExceededTimeLimit(primary);

    // Enable writes to Secondary C (unelectable). Await replication.
    // (3 out of 5 nodes caught up, 0 electable)
    // stepDown should fail due to caught up majority without electable node.
    jsTestLog("Re-enabling writes to unelectable secondary: node #" +
              replTest.getNodeId(secondaryC_unelectable) + ", " + secondaryC_unelectable);
    restartServerReplication(secondaryC_unelectable);

    // Wait for this secondary to catch up by issuing a write that must be replicated to 3 nodes
    assert.writeOK(
        coll.insert({"dummy_key": "dummy_val"}, {writeConcern: {w: 3, wtimeout: timeout}}));

    // Try to step down and fail
    jsTestLog("Trying to step down primary with a caught up majority that " +
              "doesn't contain an electable node.");
    assertStepDownFailsWithExceededTimeLimit(primary);

    // Enable writes to Secondary A (electable). Await replication.
    // (4 out of 5 nodes caught up, 1 electable)
    // stepDown should succeed due to caught up majority containing an electable node.
    jsTestLog("Re-enabling writes to electable secondary: node #" +
              replTest.getNodeId(secondaryA_electable) + ", " + secondaryA_electable);
    restartServerReplication(secondaryA_electable);

    // Wait for this secondary to catch up by issuing a write that must be replicated to 4 nodes
    assert.writeOK(
        coll.insert({"dummy_key": "dummy_val"}, {writeConcern: {w: 4, wtimeout: timeout}}));

    // Try to step down. We expect success, so catch the exception thrown by 'replSetStepDown'.
    jsTestLog("Trying to step down primary with a caught up majority that " +
              "does contain an electable node.");

    assertStepDownSucceeds(primary);

    // Make sure that original primary has transitioned to SECONDARY state
    jsTestLog("Wait for PRIMARY " + primary.host + " to completely step down.");
    replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

    // Disable all fail points for clean shutdown
    restartReplSetReplication(replTest);
    replTest.stopSet();

}());
