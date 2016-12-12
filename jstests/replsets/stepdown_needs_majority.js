/**
 * Test to ensure that 'replSetStepDown' called on a primary will fail if an electable node is
 * caught up to it but less than a majority of nodes are caught up to it. Additionally tests that
 * step down will then succeed once a majority has caught up. Tests this with a 5 node replica set.
 *
 * 1.  Initiate a 5-node replica set
 * 2.  Disable replication to all secondaries
 * 3.  Execute a write on primary with writeConcern:1
 * 4.  Try to step down primary and expect to fail
 * 5.  Enable replication to one secondary (Secondary A)
 * 6.  Await replication to Secondary A by executing primary write with writeConcern:2
 * 7.  Try to step down primary and expect failure
 * 8.  Enable replication to a different secondary (Secondary B)
 * 9.  Await replication to Secondary B by executing primary write with writeConcern:3
 * 10. Try to step down primary and expect success
 *
 */
(function() {
    load("jstests/libs/write_concern_util.js");  // for stopReplicationOnSecondaries, //
                                                 // restartServerReplication,
                                                 // restartReplSetReplication

    'use strict';

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

    function nodeIdStr(repltest, node) {
        return "node #" + repltest.getNodeId(node) + ", " + node.host;
    }

    //
    // Test setup
    //
    var name = 'stepdown_needs_majority';
    var replTest = new ReplSetTest({name: name, nodes: 5, settings: {chainingAllowed: false}});

    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var testDB = primary.getDB('testdb');
    var coll = testDB[name];
    var dummy_doc = {"dummy_key": "dummy_val"};
    var timeout = 5 * 60 * 1000;

    //
    // Block writes to all secondaries
    //
    jsTestLog("Blocking writes to all secondaries.");
    stopReplicationOnSecondaries(replTest);

    //
    // Write to the primary and attempt stepdown
    //
    jsTestLog("Issuing a write to the primary(" + primary.host + ") with write_concern:1");
    assert.writeOK(coll.insert(dummy_doc, {writeConcern: {w: 1, wtimeout: timeout}}));

    jsTestLog("Trying to step down primary with only 1 node out of 5 caught up.");
    assertStepDownFailsWithExceededTimeLimit(primary);

    //
    // Re-enable writes to Secondary A and attempt stepdown
    //
    var secondaryA = replTest.getSecondaries()[0];
    jsTestLog("Reenabling writes to one secondary (" + nodeIdStr(replTest, secondaryA) + ")");
    restartServerReplication(secondaryA);

    jsTestLog("Issuing a write to the primary with write_concern:2");
    assert.writeOK(coll.insert(dummy_doc, {writeConcern: {w: 2, wtimeout: timeout}}));

    jsTestLog("Trying to step down primary with only 2 nodes out of 5 caught up.");
    assertStepDownFailsWithExceededTimeLimit(primary);

    //
    // Re-enable writes to Secondary B and attempt stepdown
    //
    var secondaryB = replTest.getSecondaries()[1];
    jsTestLog("Reenabling writes to another secondary (" + nodeIdStr(replTest, secondaryB) + ")");
    restartServerReplication(secondaryB);

    jsTestLog("Issuing a write to the primary with write_concern:3");
    assert.writeOK(coll.insert(dummy_doc, {writeConcern: {w: 3, wtimeout: timeout}}));

    jsTestLog("Trying to step down primary with 3 nodes out of 5 caught up.");
    assertStepDownSucceeds(primary);

    jsTestLog("Waiting for PRIMARY(" + primary.host + ") to step down & become SECONDARY.");
    replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

    //
    // Disable failpoints and stop replica set
    //
    jsTestLog("Disabling all fail points to allow for clean shutdown");
    restartReplSetReplication(replTest);
    replTest.stopSet();

}());
