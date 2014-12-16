// exemplify and test the difference in meaning of w: "majority" between 2.6 and 2.8

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    var newVersion = "latest";
    var oldVersion = "2.6";

    // this first 7-node configuration will consist of 4 arbiters, 2 voting, non-arbiter nodes,
    // and 1 non-voting, non-arbiter node
    var name = "write_concern_many_arbiters";
    var nodes = {n0: {binVersion: newVersion},
                 n1: {binVersion: newVersion},
                 n2: {binVersion: oldVersion},
                 n3: {binVersion: oldVersion},
                 n4: {binVersion: oldVersion},
                 n5: {binVersion: newVersion},
                 n6: {binVersion: newVersion}};

    var replTest = new ReplSetTest({name: name, nodes: nodes});
    nodes = replTest.startSet();
    var config = {_id: name,
                  version: 1,
                  members: [{_id:0, host: nodes[0].host, priority: 3, votes: 1},
                            {_id:1, host: nodes[1].host, priority: 0, votes: 0},
                            {_id:2, host: nodes[2].host, priority: 0, votes: 1},
                            {_id:3, host: nodes[3].host, votes: 1, arbiterOnly: true},
                            {_id:4, host: nodes[4].host, votes: 1, arbiterOnly: true},
                            {_id:5, host: nodes[5].host, votes: 1, arbiterOnly: true},
                            {_id:6, host: nodes[6].host, votes: 1, arbiterOnly: true},
                           ],
                  };
    replTest.initiate(config);

    var writeConcern = {writeConcern: {w: "majority", wtimeout: 9*1000}};
    var primary = replTest.getPrimary();
    primary.forceWriteMode("commands");
    assert.eq(primary.host, nodes[0].host, "2.8 node failed to become primary");

    // w: majority on node 0 (2.8) will need both voting, non-arbiter nodes in this configuration
    // take down one voting, non-arbiter node. fails because we are missing a voter
    replTest.stop(2);
    replTest.waitForState(nodes[2], replTest.DOWN);
    assert.writeError(primary.getDB(name).foo.insert({x: 1}, writeConcern));
    // bring it back
    replTest.restart(2);
    replTest.waitForState(nodes[2], replTest.SECONDARY);
    assert.writeOK(primary.getDB(name).foo.insert({x: 2}, writeConcern));
    // take down one non-voting, non-arbiter node. passes because the non-voter does not matter
    replTest.stop(1);
    replTest.waitForState(nodes[1], replTest.DOWN);
    assert.writeOK(primary.getDB(name).foo.insert({x: 3}, writeConcern));
    replTest.restart(1);
    replTest.waitForState(nodes[1], replTest.SECONDARY);

    // reconfig such that a 2.6 node (node 2) will be primary
    config.version++;
    config.members[0].priority = 0;
    config.members[2].priority = 3;

    reconfig(replTest, config, true);
    primary = replTest.getPrimary();
    assert.eq(primary.host, nodes[2].host, "2.6 node failed to become primary");
    primary.forceWriteMode("commands");

    // w: majority on 2.6 will need all non-arbiter nodes in this configuration
    // take down one voting, non-arbiter node. fails because all nodes are needed
    replTest.stop(0);
    replTest.waitForState(nodes[0], replTest.DOWN);
    assert.writeError(primary.getDB(name).foo.insert({x: 4}, writeConcern));
    // bring it back
    replTest.restart(0);
    replTest.waitForState(nodes[0], replTest.SECONDARY);
    assert.writeOK(primary.getDB(name).foo.insert({x: 5}, writeConcern));
    // take down one non-voting, non-arbiter node. fails because all nodes are needed
    replTest.stop(1);
    replTest.waitForState(nodes[1], replTest.DOWN);
    assert.writeError(primary.getDB(name).foo.insert({x: 6}, writeConcern));

    replTest.stopSet();

    // this second 7-node configuration will consist of 1 arbiter, 4 voting, non-arbiter nodes, and
    // 2 non-voting, non-arbiter nodes
    name = "write_concern_few_arbiters";
    nodes = {n0: {binVersion: newVersion},
             n1: {binVersion: newVersion},
             n2: {binVersion: oldVersion},
             n3: {binVersion: oldVersion},
             n4: {binVersion: oldVersion},
             n5: {binVersion: newVersion},
             n6: {binVersion: newVersion}};

    replTest = new ReplSetTest({name: name, nodes: nodes});
    nodes = replTest.startSet();
    config = {_id: name,
              version: 1,
              members: [{_id:0, host: nodes[0].host, priority: 3, votes: 1},
                        {_id:1, host: nodes[1].host, priority: 0, votes: 1},
                        {_id:2, host: nodes[2].host, priority: 0, votes: 1},
                        {_id:3, host: nodes[3].host, priority: 0, votes: 1},
                        {_id:4, host: nodes[4].host, priority: 0, votes: 0},
                        {_id:5, host: nodes[5].host, priority: 0, votes: 0},
                        {_id:6, host: nodes[6].host, votes: 1, arbiterOnly: true},
                       ],
              };
    replTest.initiate(config);

    var primary = replTest.getPrimary();
    primary.forceWriteMode("commands");
    assert.eq(primary.host, nodes[0].host, "2.8 node failed to become primary");

    // w: majority on node 0 (2.8) will need 3 voting, non-arbiter nodes in this configuration
    // take down both non-voting nodes. passes because non-voting nodes do not matter
    replTest.stop(5);
    replTest.stop(4);
    replTest.waitForState(nodes[4], replTest.DOWN);
    replTest.waitForState(nodes[5], replTest.DOWN);
    assert.writeOK(primary.getDB(name).foo.insert({x: 7}, writeConcern));
    // take down one voting node. passes because we still have sufficient voting nodes
    replTest.stop(3);
    replTest.waitForState(nodes[3], replTest.DOWN);
    assert.writeOK(primary.getDB(name).foo.insert({x: 8}, writeConcern));
    // take down another voting node. fails because we have insufficient voting nodes
    replTest.stop(2);
    replTest.waitForState(nodes[2], replTest.DOWN);
    assert.writeError(primary.getDB(name).foo.insert({x: 9}, writeConcern));
    // bring back both non-voting nodes. fails because non-voters still do not matter
    replTest.restart(5);
    replTest.restart(4);
    replTest.waitForState(nodes[4], replTest.SECONDARY);
    replTest.waitForState(nodes[5], replTest.SECONDARY);
    assert.writeError(primary.getDB(name).foo.insert({x: 10}, writeConcern));
    // bring back a voting node. passes because we have sufficient voting nodes
    replTest.restart(3);
    replTest.waitForState(nodes[3], replTest.SECONDARY);
    assert.writeOK(primary.getDB(name).foo.insert({x: 11}, writeConcern));
    replTest.restart(2);
    replTest.waitForState(nodes[2], replTest.SECONDARY);


    // reconfig such that a 2.6 node (node 2) will be primary
    config.version++;
    config.members[0].priority = 0;
    config.members[2].priority = 3;

    reconfig(replTest, config, true);
    primary = replTest.getPrimary();
    assert.eq(primary.host, nodes[2].host, "2.6 node failed to become primary");
    primary.forceWriteMode("commands");

    // w: majority on 2.6 will need 4 non-arbiter nodes in this configuration
    // take down both non-voting nodes. passes because there are still sufficient nodes
    replTest.stop(5);
    replTest.stop(4);
    replTest.waitForState(nodes[4], replTest.DOWN);
    replTest.waitForState(nodes[5], replTest.DOWN);
    assert.writeOK(primary.getDB(name).foo.insert({x: 12}, writeConcern));
    // take down one voting node. fails because there are insufficient nodes
    replTest.stop(3);
    replTest.waitForState(nodes[3], replTest.DOWN);
    assert.writeError(primary.getDB(name).foo.insert({x: 13}, writeConcern));
    // bring up both non-voting nodes and take down a voting one
    // passes because only number of nodes matters in 2.6, not voting like in 2.8
    replTest.restart(5);
    replTest.restart(4);
    replTest.stop(1);
    replTest.waitForState(nodes[4], replTest.SECONDARY);
    replTest.waitForState(nodes[5], replTest.SECONDARY);
    replTest.waitForState(nodes[1], replTest.DOWN);
    assert.writeOK(primary.getDB(name).foo.insert({x: 15}, writeConcern));

    replTest.stopSet();
}());
