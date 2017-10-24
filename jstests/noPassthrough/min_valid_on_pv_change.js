// Test minValid is always set to existent OpTime on PV upgrade and downgrade,
// so that sync source resolver can find a sync source that has the minValid.

(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    var testName = "min_valid_on_pv_change";
    var replTest = new ReplSetTest({name: testName, nodes: 3});
    var nodes = replTest.nodeList();

    replTest.startSet();
    var config = {
        "_id": testName,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], priority: 0},
        ]
    };

    // Set verbosity for replication on all nodes.
    setLogVerbosity(replTest.nodes, {"replication": {"verbosity": 3}, "query": {"verbosity": 3}});

    replTest.initiate(config);
    var primary = replTest.getPrimary();
    var secondary = replTest.getSecondary();
    var coll = primary.getCollection("test.foo");

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
    // Verify the secondary can find the sync source successfully and replicate the doc.
    assert.writeOK(
        coll.insert({a: 4}, {writeConcern: {w: 3}, wtimeout: ReplSetTest.kDefaultTimeoutMS}));

})();
