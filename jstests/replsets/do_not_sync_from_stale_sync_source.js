/*
 * This tests that nodes do not sync from other nodes that are behind them. The test sets up a
 * 3-node replica set and then stops replication at one node so it starts to lag. The test then
 * uses 'replSetSyncFrom' to force the up to date node to sync from the lagging node. After it
 * receives its first batch, it errors saying that it cannot sync from a node behind it.
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    var name = "do_not_sync_from_stale_sync_source";
    var collName = "test.coll";

    var rst = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        useBridge: true,
        settings: {chainingAllowed: false}
    });
    var nodes = rst.startSet();
    rst.initiate();

    jsTestLog("Make sure node 0 is primary.");
    assert.eq(nodes[0], rst.getPrimary());
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: 0}, {writeConcern: {w: 3, wtimeout: rst.kDefaultTimeoutMS}}));

    jsTestLog("Stop node 2 from syncing so it starts lagging.");
    assert.commandWorked(nodes[2].getDB('admin').runCommand(
        {configureFailPoint: 'stopReplProducer', mode: 'alwaysOn'}));
    checkLog.contains(nodes[2], 'stopReplProducer fail point enabled');

    jsTestLog("Do a write that replicates to [0,1].");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMS}}));

    jsTestLog("Tell node 1 to sync from node 0 which is now behind.");
    assert.commandWorked(nodes[1].adminCommand({"replSetSyncFrom": nodes[0].host}));
    checkLog.contains(nodes[1], "is not greater than our last fetched OpTime");
}());
