// Test update modifier uassert during initial sync. SERVER-4781

(function doTest() {
    "use strict";

    load("jstests/replsets/rslib.js");
    var basename = "jstests_initsync4";

    jsTestLog("1. Bring up set");
    var replTest = new ReplSetTest({name: basename, nodes: 1});
    replTest.startSet();
    replTest.initiate();

    var m = replTest.getPrimary();
    var md = m.getDB("d");
    var mc = m.getDB("d")["c"];

    jsTestLog("2. Insert some data");
    var N = 5000;
    mc.ensureIndex({x: 1});
    var bulk = mc.initializeUnorderedBulkOp();
    for (var i = 0; i < N; ++i) {
        bulk.insert({_id: i, x: i, a: {}});
    }
    assert.writeOK(bulk.execute());

    jsTestLog("3. Make sure synced");
    replTest.awaitReplication();

    jsTestLog("4. Bring up a new node");
    var hostname = getHostName();

    var s = MongoRunner.runMongod({replSet: basename, oplogSize: 2});

    var config = replTest.getReplSetConfig();
    config.version = replTest.getReplSetConfigFromNode().version + 1;
    config.members.push({_id: 2, host: hostname + ":" + s.port, priority: 0});
    try {
        m.getDB("admin").runCommand({replSetReconfig: config});
    } catch (e) {
        print(e);
    }
    reconnect(s);
    assert.eq(m, replTest.getPrimary(), "Primary changed after reconfig");

    jsTestLog("5. Wait for new node to start cloning");

    s.setSlaveOk();
    var sc = s.getDB("d")["c"];

    wait(function() {
        printjson(sc.stats());
        return sc.stats().count > 0;
    });

    jsTestLog("6. Start updating documents on primary");
    for (i = N - 1; i >= N - 10000; --i) {
        // If the document is cloned as {a:1}, the {$set:{'a.b':1}} modifier will uassert.
        mc.update({_id: i}, {$set: {'a.b': 1}});
        mc.update({_id: i}, {$set: {a: 1}});
    }

    for (i = N; i < N * 2; i++) {
        mc.insert({_id: i, x: i});
    }

    assert.eq(N * 2, mc.find().itcount());

    jsTestLog("7. Wait for new node to become SECONDARY");
    wait(function() {
        var status = s.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(status);
        return status.members && (status.members[1].state == 2);
    });

    jsTestLog("8. Wait for new node to have all the data");
    wait(function() {
        return sc.find().itcount() == mc.find().itcount();
    });

    assert.eq(mc.getIndexKeys().length, sc.getIndexKeys().length);

    assert.eq(mc.find().sort({x: 1}).itcount(), sc.find().sort({x: 1}).itcount());

    replTest.stopSet(15);
}());