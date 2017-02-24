/*
 * Tests that a node on a stale branch of history won't incorrectly mark its ops as committed even
 * when hearing about a commit point with a higher optime from a new primary.
 */
(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    var name = "readCommittedStaleHistory";
    var dbName = "wMajorityCheck";
    var collName = "stepdown";

    var rst = new ReplSetTest({
        name: name,
        nodes: [
            {},
            {},
            {rsConfig: {priority: 0}},
        ],
        nodeOptions: {enableMajorityReadConcern: ""},
        useBridge: true
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = rst.nodes;
    rst.initiate();

    /**
     * Waits for the given node to be in state primary *and* have finished drain mode and thus
     * be available for writes.
     */
    function waitForPrimary(node) {
        assert.soon(function() {
            return node.adminCommand('ismaster').ismaster;
        });
    }

    // Asserts that the given document is not visible in the committed snapshot on the given node.
    function checkDocNotCommitted(node, doc) {
        var docs =
            node.getDB(dbName).getCollection(collName).find(doc).readConcern('majority').toArray();
        assert.eq(0, docs.length, tojson(docs));
    }

    // SERVER-20844 ReplSetTest starts up a single node replica set then reconfigures to the correct
    // size for faster startup, so nodes[0] is always the first primary.
    jsTestLog("Make sure node 0 is primary.");
    var primary = rst.getPrimary();
    var secondaries = rst.getSecondaries();
    assert.eq(nodes[0], primary);
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 3, wtimeout: rst.kDefaultTimeoutMS}}));

    // Stop the secondaries from replicating.
    stopServerReplication(secondaries);
    // Stop the primary from being able to complete stepping down.
    assert.commandWorked(
        nodes[0].adminCommand({configureFailPoint: 'blockHeartbeatStepdown', mode: 'alwaysOn'}));

    jsTestLog("Do a write that won't ever reach a majority of nodes");
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName).insert({a: 2}));

    // Ensure that the write that was just done is not visible in the committed snapshot.
    checkDocNotCommitted(nodes[0], {a: 2});

    // Prevent the primary from rolling back later on.
    assert.commandWorked(
        nodes[0].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'alwaysOn'}));

    jsTest.log("Disconnect primary from all secondaries");
    nodes[0].disconnect(nodes[1]);
    nodes[0].disconnect(nodes[2]);

    // Ensure the soon-to-be primary cannot see the write from the old primary.
    assert.eq(null, nodes[1].getDB(dbName).getCollection(collName).findOne({a: 2}));

    jsTest.log("Wait for a new primary to be elected");
    // Allow the secondaries to replicate again.
    restartServerReplication(secondaries);

    waitForPrimary(nodes[1]);

    jsTest.log("Do a write to the new primary");
    assert.writeOK(nodes[1].getDB(dbName).getCollection(collName).insert(
        {a: 3}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMS}}));

    // Ensure the new primary still cannot see the write from the old primary.
    assert.eq(null, nodes[1].getDB(dbName).getCollection(collName).findOne({a: 2}));

    jsTest.log("Reconnect the old primary to the rest of the nodes");
    nodes[1].reconnect(nodes[0]);
    nodes[2].reconnect(nodes[0]);

    // Sleep 10 seconds to allow some heartbeats to be processed, so we can verify that the
    // heartbeats don't cause the stale primary to incorrectly advance the commit point.
    sleep(10000);

    checkDocNotCommitted(nodes[0], {a: 2});

    jsTest.log("Allow the old primary to finish stepping down and become secondary");
    var res = null;
    try {
        res = nodes[0].adminCommand({configureFailPoint: 'blockHeartbeatStepdown', mode: 'off'});
    } catch (e) {
        // Expected - once we disable the fail point the stepdown will proceed and it's racy whether
        // the stepdown closes all connections before or after the configureFailPoint command
        // returns
    }
    if (res) {
        assert.commandWorked(res);
    }
    rst.waitForState(nodes[0], ReplSetTest.State.SECONDARY);
    reconnect(nodes[0]);

    // At this point the former primary will attempt to go into rollback, but the
    // 'rollbackHangBeforeStart' will prevent it from doing so.
    checkDocNotCommitted(nodes[0], {a: 2});
    checkLog.contains(nodes[0], 'rollback - rollbackHangBeforeStart fail point enabled');
    checkDocNotCommitted(nodes[0], {a: 2});

    jsTest.log("Allow the original primary to roll back its write and catch up to the new primary");
    assert.commandWorked(
        nodes[0].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'off'}));

    assert.soonNoExcept(function() {
        return null == nodes[0].getDB(dbName).getCollection(collName).findOne({a: 2});
    }, "Original primary never rolled back its write");

    rst.awaitReplication();

    // Ensure that the old primary got the write that the new primary did and sees it as committed.
    assert.neq(
        null,
        nodes[0].getDB(dbName).getCollection(collName).find({a: 3}).readConcern('majority').next());

    rst.stopSet();
}());
