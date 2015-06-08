// Ensure replication progress is sent upstream when initial sync completes
load("jstests/replsets/rslib.js");
(function() {
    "use strict";
    // start 1 node set
    var name = "initialSyncReportProgress";
    var ports = allocatePorts(2);
    var hostname = getHostName();

    var replSet = new ReplSetTest({name: name, nodes: 1});
    replSet.startSet();

    replSet.initiate({
        _id: name,
        members: [
            {_id: 0, host: hostname + ":" + ports[0]},
    ]});

    var primary = replSet.getPrimary();

    // do a single insert
    assert.writeOK(primary.getDB(name).foo.insert({x: 13}));
    var optime = primary.getDB(name).getLastErrorObj(1)["lastOp"];

    // start a new node and add it to the replset
    var secondary = startMongodTest(ports[1], name, false, {replSet: name, oplogSize: 25});
    var config = replSet.getReplSetConfig();
    config.version = 2;
    config.members.push({_id: 1, host: hostname + ":" + ports[1]});
    reconfig(replSet, config);
    reconnect(secondary);

    // confirm that the primary becomes aware of the new node's progress
    assert.commandWorked(primary.getDB(name).runCommand({getLastError: 1,
                                                         w: 2,
                                                         wOpTime: optime,
                                                         wTimeout: 30*1000}));
    replSet.stopSet();
})();
