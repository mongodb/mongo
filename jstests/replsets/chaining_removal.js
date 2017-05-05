// ensure removing a chained node does not break reporting of replication progress (SERVER-15849)

(function() {
    "use strict";
    load("jstests/replsets/rslib.js");

    var numNodes = 5;
    var host = getHostName();
    var name = "chaining_removal";

    var replTest = new ReplSetTest({name: name, nodes: numNodes});
    var nodes = replTest.startSet();
    var port = replTest.ports;
    replTest.initiate({
        _id: name,
        members: [
            {_id: 0, host: nodes[0].host, priority: 3},
            {_id: 1, host: nodes[1].host, priority: 0},
            {_id: 2, host: nodes[2].host, priority: 0},
            {_id: 3, host: nodes[3].host, priority: 0},
            {_id: 4, host: nodes[4].host, priority: 0},
        ],
    });
    replTest.awaitNodesAgreeOnPrimary(replTest.kDefaultTimeoutMS, nodes, 0);
    var primary = replTest.getPrimary();
    replTest.awaitReplication();

    // When setting up chaining on slow machines, we do not want slow writes or delayed heartbeats
    // to cause our nodes to invalidate the sync source provided in the 'replSetSyncFrom' command.
    // To achieve this, we disable the server parameter 'maxSyncSourceLagSecs' (see
    // repl_settings_init.cpp and TopologyCoordinatorImpl::Options) in
    // TopologyCoordinatorImpl::shouldChangeSyncSource().
    assert.commandWorked(nodes[1].getDB('admin').runCommand(
        {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'alwaysOn'}));
    assert.commandWorked(nodes[4].getDB('admin').runCommand(
        {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'alwaysOn'}));

    // Force node 1 to sync directly from node 0.
    syncFrom(nodes[1], nodes[0], replTest);
    // Force node 4 to sync through node 1.
    syncFrom(nodes[4], nodes[1], replTest);

    // write that should reach all nodes
    var timeout = 60 * 1000;
    var options = {writeConcern: {w: numNodes, wtimeout: timeout}};
    assert.writeOK(primary.getDB(name).foo.insert({x: 1}, options));

    // Re-enable 'maxSyncSourceLagSecs' checking on sync source.
    assert.commandWorked(nodes[1].getDB('admin').runCommand(
        {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'off'}));
    assert.commandWorked(nodes[4].getDB('admin').runCommand(
        {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'off'}));

    var config = primary.getDB("local").system.replset.findOne();
    config.members.pop();
    config.version++;
    // remove node 4
    replTest.stop(4);
    try {
        primary.adminCommand({replSetReconfig: config});
    } catch (e) {
        print("error: " + e);
    }

    // ensure writing to all four nodes still works
    primary = replTest.getPrimary();
    replTest.awaitReplication();
    options.writeConcern.w = 4;
    assert.writeOK(primary.getDB(name).foo.insert({x: 2}, options));

    replTest.stopSet();
}());
