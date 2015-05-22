// ensure removing a chained node does not break reporting of replication progress (SERVER-15849)

(function() {
    "use strict";
    var numNodes = 5;
    var host = getHostName();
    var name = "chaining_removal";

    var replTest = new ReplSetTest({name: name, nodes: numNodes});
    var nodes = replTest.startSet();
    var port = replTest.ports;
    replTest.initiate({_id: name, members:
            [
                {_id: 0, host: nodes[0].host, priority: 3},
                {_id: 1, host: nodes[1].host, priority: 0},
                {_id: 2, host: nodes[2].host, priority: 0},
                {_id: 3, host: nodes[3].host, priority: 0},
                {_id: 4, host: nodes[4].host, priority: 0},
            ],
            });
    var primary = replTest.getPrimary();
    // force node 4 to chain through node 1
    replTest.awaitReplication();
    assert.commandWorked(nodes[4].getDB("admin").runCommand({"replSetSyncFrom": nodes[1].host}));
    assert.commandWorked(nodes[1].getDB("admin").runCommand({"replSetSyncFrom": nodes[0].host}));
    var res;
    var res2;
    assert.soon(function() {
        res = nodes[4].getDB("admin").runCommand({"replSetGetStatus": 1});
        res2 = nodes[1].getDB("admin").runCommand({"replSetGetStatus": 1});
        return res.syncingTo === nodes[1].host && res2.syncingTo === nodes[0].host;
    }, "node 4 failed to start chaining: "+ tojson(res));

    // write that should reach all nodes
    var timeout = 15 * 1000;
    var options = {writeConcern: {w: numNodes, wtimeout: timeout}};
    assert.writeOK(primary.getDB(name).foo.insert({x: 1}, options));

    var config = primary.getDB("local").system.replset.findOne();
    config.members.pop();
    config.version++;
    // remove node 4
    replTest.stop(4);
    try {
        primary.adminCommand({replSetReconfig:config});
    }
    catch (e) {
        print("error: "+e);
    }

    // ensure writing to all four nodes still works
    primary = replTest.getPrimary();
    replTest.awaitReplication();
    options.writeConcern.w = 4;
    assert.writeOK(primary.getDB(name).foo.insert({x: 2}, options));
    
    replTest.stopSet();
}());
