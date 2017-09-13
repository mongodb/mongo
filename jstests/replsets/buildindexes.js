// Check that buildIndexes config option is working

(function() {

    var name = "buildIndexes";
    var host = getHostName();

    var replTest = new ReplSetTest({name: name, nodes: 3});

    var nodes = replTest.startSet();

    var config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    config.members[2].buildIndexes = false;

    replTest.initiate(config);

    var master = replTest.getPrimary().getDB(name);
    var slaveConns = replTest.liveNodes.slaves;
    var slave = [];
    for (var i in slaveConns) {
        slaveConns[i].setSlaveOk();
        slave.push(slaveConns[i].getDB(name));
    }
    replTest.awaitReplication();

    master.x.ensureIndex({y: 1});

    for (i = 0; i < 100; i++) {
        master.x.insert({x: 1, y: "abc", c: 1});
    }

    replTest.awaitReplication();

    assert.commandWorked(slave[0].runCommand({count: "x"}));

    var indexes = slave[0].stats().indexes;
    assert.eq(indexes, 2, 'number of indexes');

    indexes = slave[1].stats().indexes;
    assert.eq(indexes, 1);

    indexes = slave[0].x.stats().indexSizes;

    var count = 0;
    for (i in indexes) {
        count++;
        if (i == "_id_") {
            continue;
        }
        assert(i.match(/y_/));
    }

    assert.eq(count, 2);

    indexes = slave[1].x.stats().indexSizes;

    count = 0;
    for (i in indexes) {
        count++;
    }

    assert.eq(count, 1);

    replTest.stopSet();
}());
