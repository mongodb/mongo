// In this test, we verify that nodes with the delay field specified successfully wait before
// receiving writes.
// 1. We first initiate a three-node replica set with a delayed secondary and issue a
// write to the primary. We then ensure there is indeed a delay before the delayed node receives
// this write.
// 2. Next, we issue a reconfig to add a delayed fourth node, and perform the same delay
// test as above.
// 3. Finally, we perform a reconfig to reduce the fourth node's delay from 30 seconds
// to 15 seconds, and test that the delay behavior is the same as before.
//
// @tags: [
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconfig, waitForAllMembers} from "jstests/replsets/rslib.js";

let doTest = function (signal) {
    let name = "secondaryDelaySecs";
    let host = getHostName();

    let replTest = new ReplSetTest({name: name, nodes: 3});

    let nodes = replTest.startSet();
    /* set secondaryDelaySecs to 30 seconds */
    let config = replTest.getReplSetConfig();
    config.members[2].priority = 0;
    config.members[2].secondaryDelaySecs = 30;

    replTest.initiate(config);

    let primary = replTest.getPrimary().getDB(name);

    // The default WC is majority and this test can't satisfy majority writes.
    assert.commandWorked(
        primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
    );
    replTest.awaitReplication();

    let secondaryConns = replTest.getSecondaries();
    let secondaries = [];
    for (var i in secondaryConns) {
        let d = secondaryConns[i].getDB(name);
        secondaries.push(d);
    }

    waitForAllMembers(primary);

    // insert a record
    assert.commandWorked(primary.foo.insert({x: 1}, {writeConcern: {w: 2}}));

    var doc = primary.foo.findOne();
    assert.eq(doc.x, 1);

    // make sure secondary has it
    var doc = secondaries[0].foo.findOne();
    assert.eq(doc.x, 1);

    // make sure delayed secondary doesn't have it
    for (var i = 0; i < 8; i++) {
        assert.eq(secondaries[1].foo.findOne(), null);
        sleep(1000);
    }

    // within 120 seconds delayed secondary should have it
    assert.soon(
        function () {
            let z = secondaries[1].foo.findOne();
            return z && z.x == 1;
        },
        "waiting for inserted document " + tojson(doc) + " on delayed secondary",
        120 * 1000,
    );

    /************* Part 2 *******************/

    // how about if we add a new server?  will it sync correctly?
    let conn = replTest.add();

    config = primary.getSiblingDB("local").system.replset.findOne();
    printjson(config);
    config.version++;
    config.members.push({
        _id: 3,
        host: host + ":" + replTest.ports[replTest.ports.length - 1],
        priority: 0,
        secondaryDelaySecs: 30,
    });

    primary = reconfig(replTest, config);
    primary = primary.getSiblingDB(name);

    assert.commandWorked(
        primary.foo.insert({_id: 123, x: "foo"}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}),
    );

    for (var i = 0; i < 8; i++) {
        assert.eq(conn.getDB(name).foo.findOne({_id: 123}), null);
        sleep(1000);
    }

    assert.soon(function () {
        let z = conn.getDB(name).foo.findOne({_id: 123});
        return z != null && z.x == "foo";
    });

    /************* Part 3 ******************/

    print("reconfigure the delay field");

    config.version++;
    config.members[3].secondaryDelaySecs = 15;

    reconfig(replTest, config);
    primary = replTest.getPrimary().getDB(name);
    assert.soon(function () {
        return conn.getDB("local").system.replset.findOne().version == config.version;
    });

    // wait for node to become secondary
    assert.soon(function () {
        let result = conn.getDB("admin").hello();
        printjson(result);
        return result.secondary;
    });

    print("testing insert");
    primary.foo.insert({_id: 124, "x": "foo"});
    assert(primary.foo.findOne({_id: 124}) != null);

    for (var i = 0; i < 10; i++) {
        assert.eq(conn.getDB(name).foo.findOne({_id: 124}), null);
        sleep(1000);
    }

    assert.soon(function () {
        return conn.getDB(name).foo.findOne({_id: 124}) != null;
    }, "findOne should complete within default timeout");

    replTest.stopSet();
};

doTest(15);
