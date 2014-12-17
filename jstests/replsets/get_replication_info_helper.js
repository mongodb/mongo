// Tests the output of the db.getReplicationInfo() shell helper

(function () {
    "use strict";
    var name = "getReplicationInfo";
    var replSet = new ReplSetTest({name: name, nodes: 3, oplogSize: 50});
    var nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate();

    var primary = replSet.getPrimary();
    for (var i = 0; i < 100; i++) {
        primary.getDB('test').foo.insert({a:i});
    }
    replSet.awaitReplication();

    var replInfo = primary.getDB('admin').getReplicationInfo();
    var replInfoString = tojson(replInfo);

    assert.eq(50, replInfo.logSizeMB, replInfoString);
    assert.lt(0, replInfo.usedMB, replInfoString);
    assert.lt(0, replInfo.timeDiff, replInfoString);
    assert.eq(0, replInfo.timeDiffHours, replInfoString);
    // Just make sure the following fields exist since it would be hard to predict their values
    assert(replInfo.tFirst, replInfoString);
    assert(replInfo.tLast, replInfoString);
    assert(replInfo.now), replInfoString;
})();
