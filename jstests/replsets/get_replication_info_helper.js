// Tests the output of db.getReplicationInfo() and tests db.printSlaveReplicationInfo().

(function() {
    "use strict";
    var name = "getReplicationInfo";
    var replSet = new ReplSetTest({name: name, nodes: 3, oplogSize: 50});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate();

    var primary = replSet.getPrimary();
    for (var i = 0; i < 100; i++) {
        primary.getDB('test').foo.insert({a: i});
    }
    replSet.awaitReplication();

    var replInfo = primary.getDB('admin').getReplicationInfo();
    var replInfoString = tojson(replInfo);

    assert.eq(50, replInfo.logSizeMB, replInfoString);
    assert.lt(0, replInfo.usedMB, replInfoString);
    assert.lte(0, replInfo.timeDiff, replInfoString);
    assert.lte(0, replInfo.timeDiffHours, replInfoString);
    // Just make sure the following fields exist since it would be hard to predict their values
    assert(replInfo.tFirst, replInfoString);
    assert(replInfo.tLast, replInfoString);
    assert(replInfo.now, replInfoString);

    // calling this function with and without a primary, should provide sufficient code coverage
    // to catch any JS errors
    var mongo =
        startParallelShell("db.getSiblingDB('admin').printSlaveReplicationInfo();", primary.port);
    mongo();
    assert.soon(function() {
        return rawMongoProgramOutput().match("behind the primary");
    });

    // get to a primaryless state
    for (i in replSet.liveNodes.slaves) {
        var secondary = replSet.liveNodes.slaves[i];
        secondary.getDB('admin').runCommand({replSetFreeze: 120});
    }
    try {
        primary.getDB('admin').runCommand({replSetStepDown: 120, force: true});
    } catch (e) {
    }

    mongo =
        startParallelShell("db.getSiblingDB('admin').printSlaveReplicationInfo();", primary.port);
    mongo();
    assert.soon(function() {
        return rawMongoProgramOutput().match("behind the freshest");
    });

})();
