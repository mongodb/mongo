/**
 * Test killing the primary during initial sync
 * and don't allow the other secondary to become primary
 *
 * 1. Bring up set
 * 2. Insert some data
 * 4. Make sure synced
 * 5. Freeze #2
 * 6. Bring up #3
 * 7. Kill #1 in the middle of syncing
 * 8. Check that #3 makes it into secondary state
 * 9. Bring #1 back up
 * 10. Initial sync should succeed
 * 11. Ensure #1 becomes primary
 * 12. Everyone happy eventually
 */

load("jstests/replsets/rslib.js");
var basename = "jstests_initsync2";

var doTest = function() {

    jsTest.log("1. Bring up set");
    var replTest = new ReplSetTest({name: basename, nodes: [{rsConfig: {priority: 2}}, {}]});
    var conns = replTest.startSet();
    replTest.initiate();

    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

    var master = replTest.getPrimary();
    var foo = master.getDB("foo");
    var admin = master.getDB("admin");

    var slave1 = replTest.liveNodes.slaves[0];
    var admin_s1 = slave1.getDB("admin");
    var local_s1 = slave1.getDB("local");

    jsTest.log("2. Insert some data");
    for (var i = 0; i < 10000; i++) {
        foo.bar.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    jsTest.log("total in foo: " + foo.bar.find().itcount());

    jsTest.log("4. Make sure synced");
    replTest.awaitReplication();

    jsTest.log("5. Freeze #2");
    admin_s1.runCommand({replSetFreeze: 999999});

    jsTest.log("6. Bring up #3");

    var slave2 = replTest.add();
    slave2.setSlaveOk();

    var local_s2 = slave2.getDB("local");
    var admin_s2 = slave2.getDB("admin");

    replTest.reInitiate();

    reconnect(slave1);
    reconnect(slave2);

    var config = replTest.getReplSetConfigFromNode();
    jsTest.log('#1 config = ' + tojson(config));
    wait(function() {
        var config2 = local_s1.system.replset.findOne();
        var config3 = local_s2.system.replset.findOne();

        jsTest.log('#2 config = ' + tojson(config2));
        jsTest.log('#3 config = ' + tojson(config3));

        return config2.version == config.version && (config3 && config3.version == config.version);
    });
    admin_s2.runCommand({replSetFreeze: 999999});

    replTest.waitForState(replTest.nodes[2],
                          [ReplSetTest.State.SECONDARY, ReplSetTest.State.RECOVERING]);

    jsTest.log("7. Kill #1 in the middle of syncing");
    replTest.stop(0);

    jsTest.log("8. Check that #3 makes it into secondary state");
    replTest.waitForState(replTest.nodes[2],
                          [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

    jsTest.log("9. Bring #1 back up");
    replTest.start(0, {}, true);
    replTest.waitForState(replTest.nodes[0],
                          [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

    jsTest.log("10. Initial sync should succeed");
    replTest.waitForState(replTest.nodes[2],
                          [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

    jsTest.log("11. Ensure #1 becomes primary");
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);

    jsTest.log("12. Everyone happy eventually");
    replTest.awaitReplication();

    replTest.stopSet();
};

doTest();
