// Ensuring that readConcern: "majority" is not allowed with protocolVersion: 0.

(function() {
    "use strict";
    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    var replTest = new ReplSetTest(
        {"nodes": 1, "nodeOptions": {"enableMajorityReadConcern": ""}, "protocolVersion": 0});
    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        replTest.stopSet();
        return;
    }
    replTest.initiate();

    var primary = replTest.getPrimary();
    var coll = primary.getCollection("test.test_coll");
    assert.writeOK(coll.insert({_id: 1, data: "read_concern_majority_pv0_disallowed"},
                               {"w": "majority", "wtimeout": replTest.kDefaultTimeoutMS}));

    // A read concern of majority should be disallowed because protocol version is 0.
    assert.commandFailedWithCode(coll.runCommand("find", {"readConcern": {"level": "majority"}}),
                                 ErrorCodes.ReadConcernMajorityNotEnabled);

    replTest.stopSet();
}());
