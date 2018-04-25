// Test minValid is always set to existent OpTime on PV upgrade and downgrade,
// so that sync source resolver can find a sync source that has the minValid.

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    var testName = "min_valid_on_pv_change";
    var replTest =
        new ReplSetTest({name: testName, nodes: 3, nodeOptions: {enableMajorityReadConcern: ""}});
    var nodes = replTest.nodeList();

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        replTest.stopSet();
        return;
    }
    var config = {
        "_id": testName,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], priority: 0},
        ]
    };

    // Set verbosity for replication on all nodes.
    var logVerbosity = {
        replication: {verbosity: 3},
        query: {verbosity: 3},
        command: {verbosity: 1}
    };
    setLogVerbosity(replTest.nodes, logVerbosity);

    replTest.initiate(config);
    var primary = replTest.getPrimary();
    var secondary = replTest.getSecondary();
    var collNs = "test.foo";
    var coll = primary.getCollection(collNs);

    function checkMinValidExistsOnPrimary(conn, expectedTerm) {
        var minValid = conn.getDB("local").replset.minvalid.findOne();
        assert.eq(minValid.t, expectedTerm, "got unexpected minValid " + tojson(minValid));
        var lastOpTime = getLastOpTime(primary);
        if (expectedTerm == -1) {
            assert.eq(minValid.ts, lastOpTime, "got unexpected minValid " + tojson(minValid));
        } else {
            assert.docEq({t: minValid.t, ts: minValid.ts}, lastOpTime);
        }
    }

    assert.writeOK(
        coll.insert({a: 1}, {writeConcern: {w: 3}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));
    // Verify the minValid is written in PV1 so its term is 1.
    checkMinValidExistsOnPrimary(secondary, 1);

    jsTestLog("Downgrading to pv0");
    var conf = replTest.getReplSetConfigFromNode(0);
    conf.protocolVersion = 0;
    conf.version++;
    reconfig(replTest, conf);
    assert.writeOK(
        coll.insert({a: 2}, {writeConcern: {w: 3}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));
    // Verify the minValid is written in PV0 so its term is -1.
    checkMinValidExistsOnPrimary(secondary, -1);

    jsTestLog("Upgrading to pv1");
    conf.protocolVersion = 1;
    conf.version++;
    reconfig(replTest, conf);
    assert.writeOK(
        coll.insert({a: 3}, {writeConcern: {w: 3}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));
    // Verify the minValid is written in PV1 after PV upgrade so its term is 0.
    checkMinValidExistsOnPrimary(secondary, 0);

    jsTestLog("Restarting the secondary");
    replTest.restart(replTest.getSecondary());
    setLogVerbosity([replTest.getSecondary()], logVerbosity);
    // Verify the secondary can find the sync source successfully and replicate the doc.
    assert.writeOK(
        coll.insert({a: 4}, {writeConcern: {w: 3}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));

    primary.setCausalConsistency(true);
    // Check majority write works.
    assert.writeOK(coll.insert(
        {a: 5}, {writeConcern: {w: "majority"}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));

    // Check majority read works.
    jsTestLog("Checking read concern on primary");
    assert.eq(1, coll.count({a: 5}, {readConcern: "majority"}));

    jsTestLog("Checking read concern on secondary");
    var secondary = replTest.getSecondary();
    secondary.setCausalConsistency(true);
    secondary.setSlaveOk();
    assert.eq(1, secondary.getCollection(collNs).count({a: 5}, {readConcern: "majority"}));
})();
