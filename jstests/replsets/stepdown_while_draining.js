// This test ensures that a node that steps down while draining no longer deadlocks in the producer
// thread (SERVER-18994):
//
// 1) Start a 3 node set
// 2) Activate node 1's failpoint to trigger the previously problematic behavior (SERVER-18994)
// 3) Reconfig forcing node 1 to be PRIMARY
// 4) Reconfig forcing node 2 back into SECONDARY
// 5) Do a write on the new PRIMARY with w: all to ensure node 1 did not deadlock in producer thread
//
// NB: The fail point used by this test simply adds a long sleep, which greatly increases the
//     likelihood of hitting the previously bad behavior. If this test appears to be flaky on
//     EverGreen, it is entirely possible that behavior is incorrect and that the inherant raciness
//     of this test is the source of the flakiness.

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    var name = "StepDownWhileDraining";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    replTest.initiate({"_id": name,
                       "version": 1,
                       "members": [
                           { "_id": 0, "host": nodes[0], priority: 3 },
                           { "_id": 1, "host": nodes[1], priority: 0 },
                           { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                      });

    var primary = replTest.getPrimary();
    replTest.awaitReplication();
    var config = primary.getDB("local").system.replset.findOne();
    conns[1].getDB("admin").runCommand({configureFailPoint: 'stepDownWhileDrainingFailPoint',
                                        mode: 'alwaysOn'});
    config.version++;
    config.members[0].priority = 0;
    config.members[1].priority = 3;
    primary.getDB(name).foo.insert({x:1});
    reconfig(replTest, config, true);
    replTest.waitForState(replTest.nodes[1], replTest.PRIMARY, 60 * 1000);

    config = replTest.nodes[1].getDB("local").system.replset.findOne();
    config.version++;
    config.members[1].priority = 0;
    config.members[0].priority = 3;
    reconfig(replTest, config, true);
    replTest.waitForState(replTest.nodes[1], replTest.SECONDARY, 60 * 1000);

    var primary = replTest.getPrimary();
    assert.writeOK(primary.getDB(name).foo.insert({x:1},
                                                  {writeConcern: {w: 2, wtimeout: 60 * 1000}}));
}());
